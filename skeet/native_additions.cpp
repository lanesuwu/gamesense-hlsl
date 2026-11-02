#include "pch.h"
#include "native_additions.h"
#include "feature_runtime.h"
#include "feature_logic.h"
#include "startup_log.h"
#include <array>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")

namespace native_additions {
Options options;
bool accessible(const void* pointer, std::size_t length, bool executable) {
    auto address = reinterpret_cast<std::uintptr_t>(pointer);
    if (!address || !length || length > UINTPTR_MAX - address) return false;
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(pointer, &region, sizeof(region)) || region.State != MEM_COMMIT
        || (region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const auto start = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
    if (length > region.RegionSize || address - start > region.RegionSize - length) return false;
    const auto allowed = executable ? PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY
        : PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (region.Protect & allowed) != 0;
}
double now() {
    static const double frequency = [] { LARGE_INTEGER value{}; QueryPerformanceFrequency(&value); return static_cast<double>(value.QuadPart); }();
    LARGE_INTEGER value{}; QueryPerformanceCounter(&value); return value.QuadPart / frequency;
}
namespace {
using Bytes = std::vector<std::uint8_t>;
constexpr std::uintptr_t base = 0x43310000;
std::uint8_t* installed_image = nullptr;
void* menu = nullptr;
bool ui_ready = false;
void* stock_quick = nullptr;
int saved_quick = 0;
bool owns_quick = false;
std::uint8_t saved_quick_editable = 1, saved_quick_input = 1;
std::wstring settings_path;
struct ControlGate { void* object; void* parent; int Options::*condition; };
std::vector<ControlGate> control_gates;
struct Setting { const wchar_t* name; int Options::*value; int low; int high; };
const Setting settings[] = {
    {L"Grenade throw compensation", &Options::grenade_fix, 0, 1},
    {L"Custom recharge cooldown", &Options::custom_cooldown, 0, 1},
    {L"Recharge cooldown", &Options::cooldown_ms, 0, 500},
    {L"Player interpolation", &Options::interpolation, 0, 1},
    {L"Interpolation amount", &Options::interpolation_ticks, 1, 14},
    {L"Menu animations", &Options::animations, 0, 1},
    {L"Icon animation", &Options::icon_ms, 40, 350},
    {L"Tab animation", &Options::content_ms, 40, 600},
    {L"Enhance Quick Switch", &Options::quick_switch, 0, 1},
    {L"Quick switch grenades", &Options::quick_grenades, 0, 1},
    {L"Quick switch taser", &Options::quick_taser, 0, 1},
    {L"Quick switch healthshot", &Options::quick_healthshot, 0, 1}
};
void save_settings() {
    if (settings_path.empty()) return;
    for (const auto& setting : settings) {
        const auto text = std::to_wstring(options.*setting.value);
        WritePrivateProfileStringW(L"Features", setting.name, text.c_str(), settings_path.c_str());
    }
}
void load_settings() {
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&load_settings), &module)) return;
    wchar_t path[32768]{};
    auto length = GetModuleFileNameW(module, path, _countof(path));
    if (!length || length >= _countof(path)) return;
    settings_path = path;
    settings_path.resize(settings_path.find_last_of(L"\\/") + 1);
    settings_path += L"femboysense-features.ini";
    for (const auto& setting : settings) options.*setting.value = std::clamp(
        static_cast<int>(GetPrivateProfileIntW(L"Features", setting.name, options.*setting.value, settings_path.c_str())),
        setting.low, setting.high);
}
struct PointerVector { void** first; void** last; void** end; };
std::vector<void*> pointers(void* object, unsigned offset, unsigned maximum) {
    if (!accessible(object, offset + sizeof(PointerVector))) return {};
    auto value = field<PointerVector>(object, offset);
    auto first = reinterpret_cast<std::uintptr_t>(value.first), last = reinterpret_cast<std::uintptr_t>(value.last);
    if (!first || last < first || last - first > maximum * 4 || (last - first) % 4
        || value.end < value.last || !accessible(value.first, last - first)) return {};
    return std::vector<void*>(value.first, value.last);
}
std::wstring name_of(void* object) {
    if (!accessible(object, 0x40)) return {};
    auto encrypted = field<void*>(object, 0x38);
    if (!accessible(encrypted, 4)) return {};
    const auto length = field<unsigned short>(encrypted, 2);
    if (!length || length > 1024 || !accessible(encrypted, 4 + length)) return {};
    wchar_t result[256]{};
    using Decrypt = int(__fastcall*)(void*, int, wchar_t*, int);
    reinterpret_cast<Decrypt>(0x43418122)(encrypted, 2, result, _countof(result));
    result[_countof(result) - 1] = 0;
    return result;
}
void* named(const std::vector<void*>& list, const wchar_t* name) {
    for (auto entry : list) if (_wcsicmp(name_of(entry).c_str(), name) == 0) return entry;
    return nullptr;
}
void layout(void* child) {
    using Fn = void(__thiscall*)(void*);
    if (auto fn = method<Fn>(child, 8)) fn(child);
}
void sync_stock_quick() {
    if (!stock_quick || !accessible(stock_quick, 0x60)) return;
    auto value = field<int*>(stock_quick, 0x5c);
    if (!accessible(value, 4)) return;
    using Set = void(__thiscall*)(void*, bool);
    // Use the native setter so the built-in setting's callbacks also run.
    auto setter = reinterpret_cast<Set>(0x433AD3C8); // Validated before installation.
    if (options.quick_switch && enhanced_switch_active()) {
        if (!owns_quick) {
            saved_quick = *value;
            saved_quick_editable = field<std::uint8_t>(stock_quick, 0x14);
            saved_quick_input = field<std::uint8_t>(stock_quick, 0x17);
            owns_quick = true;
        }
        if (*value) setter(stock_quick, false);
        field<std::uint8_t>(stock_quick, 0x14) = 0; // Editable.
        field<std::uint8_t>(stock_quick, 0x17) = 0; // AllowMouseInput.
    } else if (owns_quick) {
        field<std::uint8_t>(stock_quick, 0x14) = saved_quick_editable;
        field<std::uint8_t>(stock_quick, 0x17) = saved_quick_input;
        // A third-party write takes ownership away from our disabled value.
        if (*value == 0) setter(stock_quick, saved_quick != 0);
        owns_quick = false;
    }
}
void __fastcall changed(void*, void*) {
    for (const auto& setting : settings) options.*setting.value = std::clamp(options.*setting.value, setting.low, setting.high);
    for (const auto& gate : control_gates) {
        if (!gate.object) continue;
        const auto visible=static_cast<std::uint8_t>((options.*gate.condition)!=0);
        if (field<std::uint8_t>(gate.object,0x15)!=visible) {
            field<std::uint8_t>(gate.object,0x15)=visible;
            layout(gate.parent);
        }
    }
    sync_stock_quick();
    gameplay_sync();
    save_settings();
}
void __fastcall open_folder(void*, void*) {
    // sub_433687C1 scans cwd/*.lua first, then cwd/lua/*.lua.
    const DWORD required = GetCurrentDirectoryW(0, nullptr);
    if (!required) return;
    std::wstring path(required, L'\0');
    if (!GetCurrentDirectoryW(required, path.data())) return;
    const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (result <= 32) startup_log::write("[features] Could not open the native Lua directory (%lld).", static_cast<long long>(result));
}
void* create(void* child, const wchar_t* text, int* value, int low = 0, int high = 1, const wchar_t* unit = L"") {
    using Allocate = void*(__thiscall*)(int);
    using Check = void*(__thiscall*)(void*, void*, const wchar_t*, int*);
    using Slider = void*(__thiscall*)(void*, void*, const wchar_t*, int, int, int*, bool, unsigned, float);
    using Insert = void(__thiscall*)(void*, bool, void*);
    auto object = reinterpret_cast<Allocate>(0x43387448)(high == 1 ? 0x78 : 0x124);
    if (!object) { startup_log::write("[features] Native control allocation failed."); return nullptr; }
    if (high == 1) reinterpret_cast<Check>(0x433ADF35)(object, child, text, value);
    else reinterpret_cast<Slider>(0x433CF6B2)(object, child, text, low, high, value, true,
        static_cast<unsigned>(unit[0]) | (unit[0] ? static_cast<unsigned>(unit[1]) << 16 : 0), 1.f);
    reinterpret_cast<Insert>(0x4336C436)(child, false, object);
    return object;
}
bool initialize_ui(void* current_menu) {
    auto tabs = pointers(current_menu, 0x54, 16);
    if (tabs.size() < 9) return false;
    auto rage = named(pointers(tabs[0], 0x70, 32), L"Other");
    auto esp = named(pointers(tabs[3], 0x70, 32), L"Player ESP");
    auto misc_children = pointers(tabs[4], 0x70, 32);
    auto misc = named(misc_children, L"Settings");
    if (!misc) misc = named(misc_children, L"Miscellaneous");
    void* scripts = nullptr; void* load = nullptr;
    for (auto tab : tabs) for (auto child : pointers(tab, 0x70, 32)) {
        if (auto found = named(pointers(child, 0x80, 1024), L"Load script")) { scripts = child; load = found; }
        if (!stock_quick) stock_quick = named(pointers(child, 0x80, 1024), L"Quick switch");
    }
    if (!rage || !esp || !misc || !scripts || !stock_quick) {
        static bool reported=false;
        if (!reported) startup_log::write("[features] Waiting for UI parents: Rage=%p ESP=%p Settings=%p Scripts=%p QuickSwitch=%p.",rage,esp,misc,scripts,stock_quick);
        reported=true;
        return false;
    }
    // Creation occurs on the native UI thread and only after every parent is present.
    menu = current_menu;
    create(rage, L"Grenade throw compensation", &options.grenade_fix);
    create(rage, L"Custom recharge cooldown", &options.custom_cooldown);
    control_gates.push_back({create(rage, L"Recharge cooldown", &options.cooldown_ms, 0, 500, L"ms"),rage,&Options::custom_cooldown});
    create(rage, L"Enhance Quick Switch", &options.quick_switch);
    control_gates.push_back({create(rage, L"Quick switch grenades", &options.quick_grenades),rage,&Options::quick_switch});
    control_gates.push_back({create(rage, L"Quick switch taser", &options.quick_taser),rage,&Options::quick_switch});
    control_gates.push_back({create(rage, L"Quick switch healthshot", &options.quick_healthshot),rage,&Options::quick_switch});
    create(esp, L"Player interpolation", &options.interpolation);
    control_gates.push_back({create(esp, L"Interpolation amount", &options.interpolation_ticks, 1, 14, L"tk"),esp,&Options::interpolation});
    create(misc, L"Menu animations", &options.animations);
    control_gates.push_back({create(misc, L"Icon animation", &options.icon_ms, 40, 350, L"ms"),misc,&Options::animations});
    control_gates.push_back({create(misc, L"Tab animation", &options.content_ms, 40, 600, L"ms"),misc,&Options::animations});
    using Button = void*(__thiscall*)(void*, void*, const wchar_t*, void*, void*);
    auto button = reinterpret_cast<Button>(0x4339E81F)(scripts, nullptr, L"Open Lua folder", nullptr, reinterpret_cast<void*>(&open_folder));
    auto entries = field<PointerVector>(scripts, 0x80);
    auto end = entries.last;
    auto at = std::find(entries.first, end, load);
    auto added = std::find(entries.first, end, button);
    if (at != end && added != end && added > at + 1) std::rotate(at + 1, added, added + 1);
    for (auto parent : {rage, esp, misc, scripts}) layout(parent);
    ui_ready = true;
    gameplay_initialize();
    changed(nullptr,nullptr);
    startup_log::write("[features] Native controls created; Enhance Quick Switch is in RAGE / Other.");
    return true;
}

struct Animation {
    int selected = -1, old = -1;
    double selection_time = 0;
    std::array<double, 16> hover_time{};
    std::array<bool, 16> hovering{};
} animation;
unsigned __cdecl icon_color(void* tab) {
    auto current_menu = field<void*>(tab, 0x60);
    if (!ui_ready) {
        static double next_attempt=0;
        if (now()>=next_attempt) { next_attempt=now()+1.; initialize_ui(current_menu); }
    }
    if (ui_ready) sync_options();
    const int index = field<int>(tab, 0x0c), selected = field<int>(current_menu, 0x64);
    const bool hover = field<std::uint8_t>(tab, 0x16) != 0;
    if (ui_ready && current_menu!=menu) return 0xffffff | ((index==selected ? 255u : hover ? 200u : 100u)<<24);
    auto time = now();
    if (animation.selected != selected) {
        animation.old = animation.selected;
        animation.selected = selected;
        animation.selection_time = animation.old >= 0 ? time : time - 1.;
    }
    unsigned alpha = index == selected ? 255 : hover ? 200 : 100;
    if (!ui_ready || !options.animations) return 0xffffff | (alpha << 24);
    if (index >= 0 && index < 16) {
        if (animation.hovering[index] && !hover) animation.hover_time[index] = time;
        animation.hovering[index] = hover;
        if (index != selected) alpha = hover ? 200 : feature_logic::blend(200, 100,
            feature_logic::progress(time - animation.hover_time[index], options.icon_ms));
    }
    const auto progress = feature_logic::progress(time - animation.selection_time, options.icon_ms);
    if (index == selected) alpha = feature_logic::blend(100, 255, progress);
    else if (index == animation.old) alpha = feature_logic::blend(255, alpha, progress);
    return 0xffffff | (alpha << 24);
}

void u32(Bytes& code, std::uintptr_t value) {
    for (int i = 0; i < 4; ++i) code.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
}
void emit(Bytes& code, std::initializer_list<std::uint8_t> bytes) { code.insert(code.end(), bytes); }
void tail(Bytes& code, std::uintptr_t destination) { code.push_back(0x68); u32(code, destination); code.push_back(0xc3); }
// Preserve GPRs, flags, x87, MXCSR and all eight SSE registers around C++ helpers.
void save_cpu(Bytes& code) {
    emit(code, {0x9c,0x60,0x8b,0xdc,0x81,0xec,0x10,0x02,0,0,0x83,0xe4,0xf0,0x0f,0xae,0x04,0x24,0xdb,0xe3});
}
void restore_cpu(Bytes& code) { emit(code, {0x0f,0xae,0x0c,0x24,0x8b,0xe3,0x61,0x9d}); }
void call_absolute(Bytes& code, void* function) { code.push_back(0xb8); u32(code, reinterpret_cast<std::uintptr_t>(function)); emit(code, {0xff,0xd0}); }
struct RendererProxy { void** table; void* original; unsigned alpha; };
struct RendererTable { void** original; std::array<void*, 43> forwarding{}; void* code = nullptr; };
std::vector<RendererTable> renderer_tables;
void** alpha_table(void* renderer) {
    if (!accessible(renderer, 4)) return nullptr;
    auto table = *static_cast<void***>(renderer);
    for (auto& entry : renderer_tables) if (entry.original == table) return entry.forwarding.data();
    if (!accessible(table, 43 * 4) || renderer_tables.size() >= 8) return nullptr;
    for (int i = 0; i < 43; ++i) if (!accessible(table[i], 1, true)) return nullptr;
    RendererTable entry{}; entry.original = table;
    auto allocation = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!allocation) return nullptr;
    entry.code = allocation;
    std::size_t offset = 0;
    for (unsigned slot = 0; slot < 43; ++slot) {
        Bytes code;
        std::vector<unsigned> colors;
        switch (slot * 4) {
        case 0x10: case 0xa4: colors = {20}; break;
        case 0x18: case 0x1c: case 0xa8: colors = {20,24}; break;
        case 0x20: colors = {20}; break;
        case 0x28: colors = {28}; break;
        case 0x3c: case 0x5c: colors = {12}; break;
        }
        if (!colors.empty()) {
            emit(code, {0x50,0x52}); // Preserve EAX and EDX (fastcall drawing flags).
            for (auto color : colors) {
                emit(code, {0x8b,0x44,0x24,static_cast<std::uint8_t>(color+8),0xc1,0xe8,0x18,0x0f,0xaf,0x41,0x08});
                // Round alpha / 255 exactly: ((x+128) + ((x+128)>>8))>>8.
                emit(code, {0x05,0x80,0,0,0,0x8b,0xd0,0xc1,0xea,0x08,0x03,0xc2,0xc1,0xe8,0x08,
                    0x88,0x44,0x24,static_cast<std::uint8_t>(color+11)});
            }
            emit(code, {0x5a,0x58});
        }
        emit(code, {0x8b,0x49,0x04}); // Restore real renderer this, even for unmodified slots.
        tail(code, reinterpret_cast<std::uintptr_t>(table[slot]));
        if (offset + code.size() > 4096) { VirtualFree(allocation,0,MEM_RELEASE); return nullptr; }
        entry.forwarding[slot] = allocation + offset;
        std::memcpy(allocation + offset, code.data(), code.size());
        offset = (offset + code.size() + 15) & ~std::size_t(15);
    }
    DWORD previous{};
    if (!VirtualProtect(allocation,4096,PAGE_EXECUTE_READ,&previous) || !FlushInstructionCache(GetCurrentProcess(),allocation,4096)) {
        VirtualFree(allocation,0,MEM_RELEASE); return nullptr;
    }
    renderer_tables.push_back(entry);
    return renderer_tables.back().forwarding.data();
}
int __fastcall draw_content(void* child, void*, void* renderer, void* position) {
    using Draw = int(__thiscall*)(void*, void*, void*);
    auto draw = method<Draw>(child, 4);
    if (!draw) return 0;
    unsigned alpha = 255;
    auto tab=accessible(child,0x14) ? field<void*>(child,0x10) : nullptr;
    const bool main_menu_child=accessible(tab,0x64) && field<void*>(tab,0x60)==menu;
    if (ui_ready && main_menu_child && options.animations) alpha = feature_logic::blend(0, 255,
        feature_logic::progress(now() - animation.selection_time, options.content_ms));
    if (alpha == 255) return draw(child, renderer, position);
    auto table = alpha_table(renderer);
    if (!table) return draw(child, renderer, position);
    RendererProxy proxy{table, renderer, alpha};
    // The proxy and alpha exist only for this synchronous menu-child draw call.
    // Native renderer vtables and unrelated ESP/blur calls are never changed.
    return draw(child, &proxy, position);
}
unsigned __cdecl transform_pitch(float* saved_xmm0, int inverse) {
    float value = *saved_xmm0;
    if (options.grenade_fix) value = inverse ? feature_logic::inverse_throw_pitch(value) : feature_logic::throw_pitch(value);
    unsigned bits; std::memcpy(&bits, &value, 4); return bits;
}
float __cdecl cooldown() { return feature_logic::cooldown_seconds(options.custom_cooldown != 0, options.cooldown_ms); }
struct Patch { std::uintptr_t address; Bytes original; Bytes replacement; };
void jump_patch(std::vector<Patch>& patches, std::uintptr_t address, Bytes original, std::uintptr_t destination) {
    Bytes jump{0xe9}; u32(jump, destination - address - 5); jump.resize(original.size(),0x90);
    patches.push_back({address,std::move(original),std::move(jump)});
}
}

void sync_options() {
    if (!ui_ready) return;
    static Options previous = options;
    if (std::memcmp(&previous, &options, sizeof(options))) {
        changed(nullptr, nullptr);
        previous = options;
    }
    sync_stock_quick();
}

bool install(std::uint8_t* image, std::size_t size) {
    static_assert(sizeof(void*) == 4);
    if (installed_image) return installed_image == image;
    if (reinterpret_cast<std::uintptr_t>(image) != base || size < 0x200000 || !accessible(image,size)) return false;
    MEMORY_BASIC_INFORMATION mapping{};
    if (!VirtualQuery(image,&mapping,sizeof(mapping))
        || !(mapping.Protect & (PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY))) return false;
    // Validate fixed-build constructors and all complete stolen instructions.
    struct Anchor { std::uintptr_t at; const char* bytes; std::size_t count; };
    const Anchor anchors[] = {
        {0x43387448,"\x56\x8b\xf1\x33\xc0\x85",6}, {0x433ADF35,"\x56\xff\x74\x24\x10\x8b\xf1\x6a",8},
        {0x433CF6B2,"\x55\x8b\xec\x53\x56\xff",6}, {0x4336C436,"\x56\x57\x8b\xf9\x8b\x4f\x10",7},
        {0x4339E81F,"\x55\x8b\xec\x83\xe4\xf8",6}, {0x43418122,"\x51\x51\x55\x56\x8b\xc2",6}
        ,{0x433AD3C8,"\x8b\x41\x5c\x53\x85",5}
    };
    for (const auto& anchor : anchors) if (std::memcmp(image+anchor.at-base,anchor.bytes,anchor.count)) {
        startup_log::write("[features] Constructor validation failed at %p; additions not installed.",reinterpret_cast<void*>(anchor.at)); return false;
    }
    load_settings();
    // Fixed-capacity cache: proxy vtable addresses must survive later insertions.
    renderer_tables.reserve(8);
    auto cave = static_cast<std::uint8_t*>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    if (!cave) return false;
    std::vector<Patch> patches;
    std::size_t used = 0;
    bool code_failed = false;
    auto add_code = [&](const Bytes& bytes) {
        auto address = reinterpret_cast<std::uintptr_t>(cave + used);
        if (used + bytes.size() > 4096) { code_failed=true; return std::uintptr_t(0); }
        std::memcpy(cave+used,bytes.data(),bytes.size()); used=(used+bytes.size()+15)&~std::size_t(15); return address;
    };
    Bytes icon; save_cpu(icon); icon.push_back(0x57); call_absolute(icon,reinterpret_cast<void*>(&icon_color));
    emit(icon,{0x83,0xc4,0x04,0x89,0x43,0x1c}); restore_cpu(icon); emit(icon,{0x89,0x44,0x24,0x08}); tail(icon,0x433AC676);
    jump_patch(patches,0x433AC656,{0x3b,0xc1,0x75,0x07,0x83,0x4c,0x24,0x08,0xff,0xeb,0x15,0x80,0x7f,0x16,0x00,0xba,
        0xff,0xff,0xff,0xc8,0xb8,0xff,0xff,0xff,0x64,0x0f,0x44,0xd0,0x89,0x54,0x24,0x08},add_code(icon));
    Bytes content; emit(content,{0xff,0x75,0x0c,0xff,0x75,0x08}); call_absolute(content,reinterpret_cast<void*>(&draw_content)); tail(content,0x433AC78D);
    jump_patch(patches,0x433AC782,{0xff,0x75,0x0c,0x8b,0x01,0xff,0x75,0x08,0xff,0x50,0x10},add_code(content));
    for (int inverse = 0; inverse < 2; ++inverse) {
        const auto address = inverse ? 0x433E79A7u : 0x433E76F2u;
        Bytes original{0xf3,0x0f,0x59,0x05}; u32(original,inverse ? 0x4346DD48 : 0x43470528);
        Bytes code;
        if (inverse) emit(code,{0xf3,0x0f,0x59,0x05}), u32(code,0x4346DD48);
        save_cpu(code);
        // FXSAVE's XMM0 low scalar is at +160; save base before pushing arguments.
        emit(code,{0x8d,0x84,0x24,0xa0,0,0,0,0x6a,static_cast<std::uint8_t>(inverse),0x50});
        call_absolute(code,reinterpret_cast<void*>(&transform_pitch));
        emit(code,{0x83,0xc4,0x08,0x89,0x84,0x24,0xa0,0,0,0}); restore_cpu(code);
        if (!inverse) emit(code,{0xf3,0x0f,0x59,0x05}), u32(code,0x43470528);
        tail(code,address+8); jump_patch(patches,address,original,add_code(code));
    }
    // Keep executable bytes immutable after checksum preparation. Each timer
    // store reads the option at execution time, leaving the recharge gate intact.
    for (int second = 0; second < 2; ++second) {
        auto address = second ? 0x43366F22u : 0x43359F00u;
        Bytes original{0xc7,static_cast<std::uint8_t>(second ? 0x87 : 0x82),0x24,0x01,0,0,0,0,0,0x3f};
        Bytes code; save_cpu(code); call_absolute(code,reinterpret_cast<void*>(&cooldown));
        // Win32 returns scalar float in ST(0). Extract before FXRSTOR restores it.
        emit(code,{0xd9,0x9c,0x24,0x00,0x02,0,0,0x8b,0x84,0x24,0x00,0x02,0,0});
        // Store through the original destination, without changing EAX on return.
        emit(code,{0x8b,0x4b,static_cast<std::uint8_t>(second ? 0x00 : 0x14),0x89,0x81,0x24,0x01,0,0});
        restore_cpu(code); tail(code,address+10); jump_patch(patches,address,original,add_code(code));
    }
    if (code_failed) { VirtualFree(cave,0,MEM_RELEASE); return false; }
    for (const auto& patch : patches) if (std::memcmp(image+patch.address-base,patch.original.data(),patch.original.size())) {
        startup_log::write("[features] Patch conflict at %p; no additions applied.",reinterpret_cast<void*>(patch.address));
        VirtualFree(cave,0,MEM_RELEASE); return false;
    }
    DWORD old{};
    if (!VirtualProtect(cave,4096,PAGE_EXECUTE_READ,&old) || !FlushInstructionCache(GetCurrentProcess(),cave,4096)) {
        VirtualFree(cave,0,MEM_RELEASE); return false;
    }
    installed_image = image;
    for (const auto& patch : patches) std::memcpy(image+patch.address-base,patch.replacement.data(),patch.replacement.size());
    if (!FlushInstructionCache(GetCurrentProcess(),image,size)) {
        for (const auto& patch : patches) std::memcpy(image+patch.address-base,patch.original.data(),patch.original.size());
        FlushInstructionCache(GetCurrentProcess(),image,size); installed_image=nullptr; VirtualFree(cave,0,MEM_RELEASE); return false;
    }
    // Globals pointer used by the image itself (CGlobalVarsBase, interval +0x20).
    const unsigned char signature[] = {0x8b,0x0d,0,0,0,0,0x8b,0x54,0x24,0x20,0xd8,0x71,0x20};
    for (unsigned i=0xe000;i+sizeof(signature)<0x114000;++i) if (!std::memcmp(image+i,signature,2)
        && !std::memcmp(image+i+6,signature+6,sizeof(signature)-6)) {
        std::uintptr_t binding; std::memcpy(&binding,image+i+2,4); set_globals_binding(binding); break;
    }
    startup_log::write("[features] Native UI/animation, grenade and cooldown hooks installed before checksum capture.");
    return true;
}
}
