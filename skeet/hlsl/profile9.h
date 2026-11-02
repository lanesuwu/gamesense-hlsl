#pragma once

#include "../Json/json.hpp"
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstdint>

// Opt-in, process-wide CPU measurements. No allocations, logging, blocking
// locks or GPU flushes occur at a measurement boundary.
namespace profile9 {
enum Stage {
    MemoryRequest,
    Decode,
    Reply,
    BackendRequest,
    Submit,
    Render,
    RenderLock,
    Capture,
    StateSave,
    StateRestore,
    Pass,
    Bindings,
    Upload,
    Timing,
    HookRepair,
    Count
};
inline constexpr const char *names[] = {"memory_request", "decode",        "reply",       "backend_request",
                                        "submit",         "render",        "render_lock", "capture",
                                        "state_save",     "state_restore", "pass",        "bindings",
                                        "upload",         "timing",        "hook_repair"};
struct Aggregate {
    std::atomic<uint64_t> count{0}, ticks{0}, maximum{0};
};
inline std::array<Aggregate, Count> totals;
inline std::atomic<bool> enabled{false};
inline std::atomic<unsigned> epoch{0};
inline uint64_t now() {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return uint64_t(value.QuadPart);
}
inline void record(Stage stage, uint64_t ticks) {
    auto &value = totals[stage];
    value.count.fetch_add(1, std::memory_order_relaxed);
    value.ticks.fetch_add(ticks, std::memory_order_relaxed);
    auto maximum = value.maximum.load(std::memory_order_relaxed);
    while (maximum < ticks &&
           !value.maximum.compare_exchange_weak(maximum, ticks, std::memory_order_relaxed)) {
    }
}
inline void restart() {
    epoch.fetch_add(1, std::memory_order_relaxed);
    for (auto &value : totals) {
        value.count.store(0, std::memory_order_relaxed);
        value.ticks.store(0, std::memory_order_relaxed);
        value.maximum.store(0, std::memory_order_relaxed);
    }
}
struct Scope {
    Stage stage;
    uint64_t start = 0;
    unsigned generation = 0;
    bool active;
    explicit Scope(Stage value) : stage(value), active(enabled.load(std::memory_order_relaxed)) {
        if (active) {
            generation = epoch.load(std::memory_order_relaxed);
            start = now();
        }
    }
    ~Scope() {
        if (active && generation == epoch.load(std::memory_order_relaxed))
            record(stage, now() - start);
    }
};
inline nlohmann::json snapshot() {
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    const double scale = 1000.0 / double(frequency.QuadPart);
    auto result = nlohmann::json::object();
    for (unsigned i = 0; i < Count; ++i) {
        const auto count = totals[i].count.load(std::memory_order_relaxed);
        result[names[i]] = {
            {"samples", count},
            {"mean_ms",
             count ? double(totals[i].ticks.load(std::memory_order_relaxed)) * scale / double(count) : 0},
            {"max_ms", double(totals[i].maximum.load(std::memory_order_relaxed)) * scale}};
    }
    return result;
}
} // namespace profile9
