#include "pch.h"
#include "feature_runtime.h"
#include "startup_log.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>

namespace native_additions {
namespace {
void* engine = nullptr;
void* client = nullptr;
void* entities = nullptr;
void* events = nullptr;
void* infinite_ammo = nullptr;
std::uintptr_t globals_binding = 0;
using Stage = void(__thiscall*)(void*, int);
Stage original_stage = nullptr;
bool initialized = false, listening = false;
int health_offset = 0, active_weapon_offset = 0, life_state_offset = 0, item_definition_offset = 0;

void* interface_from(const wchar_t* module, const char* name) {
    auto handle = GetModuleHandleW(module);
    using Create = void*(__cdecl*)(const char*, int*);
    auto create = reinterpret_cast<Create>(handle ? GetProcAddress(handle,"CreateInterface") : nullptr);
    return create ? create(name,nullptr) : nullptr;
}
bool in_game() {
    using Fn = bool(__thiscall*)(void*);
    auto fn = method<Fn>(engine,26); return fn && fn(engine);
}
int local_index() {
    using Fn = int(__thiscall*)(void*);
    auto fn = method<Fn>(engine,12); return fn ? fn(engine) : 0;
}
void* entity_at(int index) {
    using Fn = void*(__thiscall*)(void*,int);
    auto fn = method<Fn>(entities,3); return fn && index > 0 ? fn(entities,index) : nullptr;
}
bool alive(void* entity) {
    return entity && health_offset && life_state_offset
        && accessible(entity,static_cast<std::size_t>(std::max(health_offset+4,life_state_offset+1)))
        && field<int>(entity,health_offset) > 0 && field<std::uint8_t>(entity,life_state_offset) == 0;
}
unsigned current_weapon(void* entity) {
    return entity && active_weapon_offset && accessible(entity,active_weapon_offset+4)
        ? field<unsigned>(entity,active_weapon_offset) : 0xffffffff;
}
std::uint32_t tick_count() {
    if (!accessible(reinterpret_cast<void*>(globals_binding),4)) return 0;
    auto globals = *reinterpret_cast<void**>(globals_binding);
    return accessible(globals,0x24) ? field<std::uint32_t>(globals,0x1c) : 0;
}

struct RecvTable;
struct RecvProp {
    const char* name; int type,flags,string_size; bool in_array; char pad[3];
    void* extra; void* array; void* length_proxy; void* proxy; void* table_proxy;
    RecvTable* table; int offset,stride,count; const char* parent;
};
struct RecvTable { RecvProp* props; int count; void* decoder; const char* name; };
struct ClientClass { void* create; void* create_event; const char* name; RecvTable* table; ClientClass* next; int id; };
static_assert(sizeof(RecvProp)==60);
bool text_equal(const char* text, const char* expected) {
    const auto length = std::strlen(expected)+1;
    return accessible(text,length) && std::memcmp(text,expected,length)==0;
}
int prop(RecvTable* table, const char* name, unsigned depth = 0) {
    if (depth > 12 || !accessible(table,sizeof(RecvTable)) || table->count < 0 || table->count > 2048
        || !accessible(table->props,table->count*sizeof(RecvProp))) return 0;
    for (int i=0;i<table->count;++i) {
        const auto& entry=table->props[i];
        if (entry.offset<0 || entry.offset>0x20000) continue;
        if (text_equal(entry.name,name)) return entry.offset;
        if (entry.table) if (auto nested=prop(entry.table,name,depth+1)) return entry.offset+nested;
    }
    return 0;
}
void discover_props() {
    using Classes = ClientClass*(__thiscall*)(void*);
    auto fn=method<Classes>(client,8);
    if (!fn) return;
    auto entry=fn(client);
    for (int i=0;entry && i<1024;++i) {
        if (!accessible(entry,sizeof(ClientClass))) break;
        if (text_equal(entry->name,"CCSPlayer")) {
            health_offset=prop(entry->table,"m_iHealth");
            life_state_offset=prop(entry->table,"m_lifeState");
            active_weapon_offset=prop(entry->table,"m_hActiveWeapon");
        }
        if (!item_definition_offset) item_definition_offset=prop(entry->table,"m_iItemDefinitionIndex");
        if (health_offset && life_state_offset && active_weapon_offset && item_definition_offset) break;
        entry=entry->next;
    }
    startup_log::write("[features] Netvars: health=%x life=%x active_weapon=%x item_definition=%x.",health_offset,life_state_offset,active_weapon_offset,item_definition_offset);
}

struct VarEntry { std::uint16_t type,needs_interpolate; void* data; void* watcher; };
struct Watcher {
    void* entity=nullptr; void* watcher=nullptr; void* data=nullptr; void* setter=nullptr;
    float original=0, written=0;
};
std::array<Watcher,65> watchers;
VarEntry* origin_entry(void* entity) {
    // The supplied client has FIELD_VECTOR m_vecOrigin at datamap offset 0xAC;
    // its network m_vecOrigin is a separate field at 0x138. Do not substitute it.
    if (!accessible(entity,0x3c)) return nullptr;
    auto first=field<VarEntry*>(entity,0x24);
    auto count=field<int>(entity,0x38);
    if (count<1 || count>128 || !accessible(first,count*sizeof(VarEntry))) return nullptr;
    for (int i=0;i<count;++i) {
        if (first[i].data==static_cast<std::uint8_t*>(entity)+0xac && accessible(first[i].data,12)
            && accessible(first[i].watcher,0x10)) return first+i;
    }
    return nullptr;
}
void* verified_setter(VarEntry* entry) {
    using Set=void(__thiscall*)(void*,float);
    auto fn=method<Set>(entry->watcher,2);
    // Verified in the installed legacy client: mov eax,[esp+4];
    // mov [ecx+0x0c],eax; ret 4. This also establishes where to save the native amount.
    static const unsigned char bytes[]={0x8b,0x44,0x24,0x04,0x89,0x41,0x0c,0xc2,0x04,0x00};
    if (!accessible(reinterpret_cast<void*>(fn),sizeof(bytes),true)
        || std::memcmp(reinterpret_cast<void*>(fn),bytes,sizeof(bytes))) return nullptr;
    return reinterpret_cast<void*>(fn);
}
void restore(Watcher& saved, VarEntry* current, void* entity) {
    if (saved.watcher && saved.entity==entity && current && current->watcher==saved.watcher && current->data==saved.data
        && verified_setter(current)==saved.setter) {
        const float value=field<float>(saved.watcher,0x0c);
        // Restore only an amount we still own. Engine/server updates win.
        if (value==saved.written) reinterpret_cast<void(__thiscall*)(void*,float)>(saved.setter)(saved.watcher,saved.original);
    }
    saved={};
}
void update_interpolation(bool enabled) {
    if (!enabled && std::none_of(watchers.begin(),watchers.end(),[](const Watcher& item) { return item.watcher!=nullptr; })) return;
    auto local=local_index();
    const float amount=tick_interval()*std::clamp(options.interpolation_ticks,1,14);
    for (int index=1;index<=64;++index) {
        auto& saved=watchers[index]; auto entity=entity_at(index);
        auto entry=origin_entry(entity);
        if (!enabled || index==local || !alive(entity) || !entry) { restore(saved,entry,entity); continue; }
        auto setter=verified_setter(entry);
        auto value=field<float>(entry->watcher,0x0c);
        if (!setter || !std::isfinite(value) || value<0 || value>2) { restore(saved,entry,entity); continue; }
        if (saved.entity!=entity || saved.watcher!=entry->watcher || saved.setter!=setter) {
            restore(saved,entry,entity);
            saved={entity,entry->watcher,entry->data,setter,value,amount};
        } else if (value!=saved.written) saved.original=value;
        reinterpret_cast<void(__thiscall*)(void*,float)>(setter)(entry->watcher,amount);
        saved.written=amount;
    }
}

enum class SwitchReason { none, grenade, taser, healthshot };
struct PendingSwitch {
    SwitchReason reason=SwitchReason::none;
    void* owner=nullptr;
    unsigned weapon=0xffffffff;
    std::uint32_t tick=0;
    double earliest=0, expires=0;
    int health=0;
} pending;
void cancel_switch() { pending={}; }
bool reason_enabled(SwitchReason reason) {
    return options.quick_switch && (reason==SwitchReason::grenade ? options.quick_grenades
        : reason==SwitchReason::taser ? options.quick_taser : reason==SwitchReason::healthshot ? options.quick_healthshot : false);
}
void schedule(SwitchReason reason, void* owner) {
    if (!reason_enabled(reason) || !alive(owner)) return;
    const auto delay=reason==SwitchReason::grenade ? tick_interval() : 0.f;
    pending={reason,owner,current_weapon(owner),tick_count(),now()+delay,now()+3.,field<int>(owner,health_offset)};
}
void process_switch() {
    if (pending.reason==SwitchReason::none) return;
    auto me=entity_at(local_index());
    if (!reason_enabled(pending.reason) || !in_game() || me!=pending.owner || !alive(me) || now()>pending.expires) { cancel_switch(); return; }
    // Respect a deliberate weapon change while a delayed action was outstanding.
    const auto weapon=current_weapon(me);
    if (weapon!=pending.weapon && weapon!=0xffffffff && pending.weapon!=0xffffffff) {
        using FromHandle=void*(__thiscall*)(void*,unsigned);
        auto from=method<FromHandle>(entities,4);
        auto held=from ? from(entities,weapon) : nullptr;
        auto item=item_definition_offset && accessible(held,item_definition_offset+2)
            ? field<std::uint16_t>(held,item_definition_offset) : 0;
        // A consumed grenade/taser can automatically select a knife or another
        // grenade. Still return to a firearm; preserve an already-selected gun.
        const bool automatic_utility=item==42 || item==59 || (item>=43 && item<=48) || (item>=500 && item<=525);
        if (!automatic_utility || pending.reason==SwitchReason::healthshot) { cancel_switch(); return; }
    }
    if (pending.reason==SwitchReason::healthshot && field<int>(me,health_offset)<=pending.health) return;
    if (now()<pending.earliest || (pending.reason==SwitchReason::grenade && tick_count()==pending.tick)) return;
    using Command=void(__thiscall*)(void*,const char*);
    // Same engine slot used by the bundled SDK's CEngine::ExecCommand.
    auto execute=method<Command>(engine,108);
    cancel_switch();
    if (execute) execute(engine,"slot3; slot2; slot1");
}
class Listener {
public:
    virtual ~Listener()=default;
    virtual void FireGameEvent(void* event) {
        using Name=const char*(__thiscall*)(void*);
        using Int=int(__thiscall*)(void*,const char*,int);
        using String=const char*(__thiscall*)(void*,const char*,const char*);
        auto name_fn=method<Name>(event,1);
        auto weapon_fn=method<String>(event,9);
        auto integer=method<Int>(event,6);
        if (!name_fn || !integer || !weapon_fn) return;
        auto name=name_fn(event);
        if (text_equal(name,"round_start") || text_equal(name,"game_newmap")) { cancel_switch(); return; }
        using User=int(__thiscall*)(void*,int);
        auto user=method<User>(engine,9);
        if (!user || user(engine,integer(event,"userid",0))!=local_index()) return;
        if (text_equal(name,"player_death")) { cancel_switch(); return; }
        auto me=entity_at(local_index());
        if (text_equal(name,"grenade_thrown")) { schedule(SwitchReason::grenade,me); return; }
        if (!text_equal(name,"weapon_fire")) return;
        auto weapon=weapon_fn(event,"weapon","");
        if (text_equal(weapon,"taser") || text_equal(weapon,"weapon_taser")) {
            using IntValue=int(__thiscall*)(void*);
            auto get=method<IntValue>(infinite_ammo,13);
            if (get && get(infinite_ammo)==1) return;
            schedule(SwitchReason::taser,me);
        } else if (text_equal(weapon,"healthshot") || text_equal(weapon,"weapon_healthshot")) {
            schedule(SwitchReason::healthshot,me);
        } else if (text_equal(weapon,"default")) {
            // The event sometimes reports "default" for the healthshot. Validate
            // the held entity's network class before treating it as one.
            using FromHandle=void*(__thiscall*)(void*,unsigned);
            auto from=method<FromHandle>(entities,4);
            auto held=from ? from(entities,current_weapon(me)) : nullptr;
            using Networkable=void*(__thiscall*)(void*);
            auto get_networkable=method<Networkable>(held,4);
            auto networkable=get_networkable ? get_networkable(held) : nullptr;
            using GetClass=ClientClass*(__thiscall*)(void*);
            auto cls=method<GetClass>(networkable,2);
            auto info=cls ? cls(networkable) : nullptr;
            if (accessible(info,sizeof(ClientClass)) && text_equal(info->name,"CWeaponHealthshot")) schedule(SwitchReason::healthshot,me);
        }
    }
    virtual int GetEventDebugID() { return 42; }
} listener;

void __fastcall frame_stage(void* self, void*, int stage) {
    // Restore before native packet processing can replace the amount or recycle
    // entities. Reapply only at FRAME_NET_UPDATE_END, after native updates finish.
    if (stage==1 && in_game()) update_interpolation(false);
    if (original_stage) original_stage(self,stage);
    if (stage==0) {
        sync_options(); gameplay_sync();
        if (!in_game()) { cancel_switch(); for (auto& item:watchers) item={}; }
        else process_switch();
    }
    if (stage==4 && in_game()) {
        update_interpolation(options.interpolation!=0);
        process_switch();
    }
}
bool install_stage() {
    auto existing=method<Stage>(client,37);
    if (!existing) return false;
    auto table=*static_cast<void***>(client);
    DWORD previous{};
    if (!VirtualProtect(table+37,4,PAGE_READWRITE,&previous)) return false;
    original_stage=existing;
    table[37]=reinterpret_cast<void*>(&frame_stage);
    DWORD ignored{};
    if (!VirtualProtect(table+37,4,previous,&ignored)) {
        table[37]=reinterpret_cast<void*>(existing);
        VirtualProtect(table+37,4,previous,&ignored); original_stage=nullptr; return false;
    }
    return true;
}
}
void set_globals_binding(std::uintptr_t address) { globals_binding=address; }
float tick_interval() {
    if (accessible(reinterpret_cast<void*>(globals_binding),4)) {
        auto globals=*reinterpret_cast<void**>(globals_binding);
        if (accessible(globals,0x24)) {
            auto value=field<float>(globals,0x20);
            if (std::isfinite(value) && value>=.001f && value<=.1f) return value;
        }
    }
    return 1.f/64.f;
}
void gameplay_initialize() {
    if (initialized) return;
    engine=interface_from(L"engine.dll","VEngineClient014");
    client=interface_from(L"client.dll","VClient018");
    entities=interface_from(L"client.dll","VClientEntityList003");
    events=interface_from(L"engine.dll","GAMEEVENTSMANAGER002");
    auto cvars=interface_from(L"vstdlib.dll","VEngineCvar007");
    using Find=void*(__thiscall*)(void*,const char*);
    if (auto find=method<Find>(cvars,15)) infinite_ammo=find(cvars,"sv_infinite_ammo");
    if (!engine || !client || !entities || !events || !install_stage()) {
        startup_log::write("[features] Gameplay interfaces not verified; interpolation/Quick Switch inactive."); return;
    }
    initialized=true;
    discover_props();
    gameplay_sync();
    startup_log::write("[features] Frame-stage callback installed; origin watcher layout checked before each write.");
}
void gameplay_sync() {
    if (!initialized) return;
    if (!options.interpolation && in_game()) update_interpolation(false);
    const bool enabled=options.quick_switch && health_offset && life_state_offset && active_weapon_offset;
    if (!enabled) cancel_switch();
    if (enabled==listening) return;
    using Add=bool(__thiscall*)(void*,void*,const char*,bool);
    using Remove=void(__thiscall*)(void*,void*);
    auto add=method<Add>(events,3);
    auto remove=method<Remove>(events,5);
    if (!add || !remove) return;
    if (enabled) {
        for (auto name : {"weapon_fire","grenade_thrown","player_death","round_start","game_newmap"}) {
            if (!add(events,&listener,name,false)) {
                remove(events,&listener);
                startup_log::write("[features] Quick Switch event registration failed for %s.",name); return;
            }
        }
        listening=true;
    } else { remove(events,&listener); listening=false; }
}
bool enhanced_switch_active() { return initialized && listening; }
}
