#pragma once
#include <Windows.h>
#include <cstddef>
#include <cstdint>

namespace native_additions {
struct Options {
    int grenade_fix = 0;
    int custom_cooldown = 0;
    int cooldown_ms = 500;
    int interpolation = 0;
    int interpolation_ticks = 1;
    int animations = 1;
    int icon_ms = 350;
    int content_ms = 250;
    int quick_switch = 0;
    int quick_grenades = 1;
    int quick_taser = 1;
    int quick_healthshot = 1;
};
extern Options options;
bool accessible(const void* pointer, std::size_t length, bool executable = false);
double now();
void gameplay_initialize();
void gameplay_sync();
bool enhanced_switch_active();
void sync_options();
float tick_interval();
void set_globals_binding(std::uintptr_t address);
template<typename T> T& field(void* object, std::size_t offset) {
    return *reinterpret_cast<T*>(static_cast<std::uint8_t*>(object) + offset);
}
template<typename T> T method(void* object, unsigned index) {
    if (!accessible(object, 4)) return nullptr;
    auto table = *static_cast<void***>(object);
    if (!accessible(table, (index + 1) * sizeof(void*)) || !accessible(table[index], 1, true)) return nullptr;
    return reinterpret_cast<T>(table[index]);
}
}
