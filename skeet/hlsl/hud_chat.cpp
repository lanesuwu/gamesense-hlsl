#include "hud_chat.h"
#include "protocol_guard.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace hud_chat {
namespace {
using J = nlohmann::json;
using Print = void(__cdecl *)(void *, int, int, const char *, ...);
using PrintEx = void(__cdecl *)(void *, int, int, int, const char *, ...);
using Filter = unsigned(__thiscall *)(void *);
constexpr unsigned capacity = 128, textCapacity = 4096, batchLimit = 32;
constexpr ULONGLONG leaseMilliseconds = 1000;
constexpr DWORD clientStamp = 0x65272025, clientImageSize = 0x5509000;
constexpr std::uintptr_t chatPointerRva = 0x5334b18;
constexpr std::uintptr_t printRva = 0x2dabb0, printExRva = 0x2daad0, filterRva = 0x2d65b0;
constexpr unsigned char printPrefix[] = {0x55, 0x8b, 0xec, 0xb8, 0x00, 0x10, 0x00, 0x00};

struct Line {
    std::uint64_t serial = 0;
    int player = 0, filter = 0;
    std::array<char, textCapacity> text{};
};
struct Lease {
    unsigned owner = 0;
    bool hide = false;
    ULONGLONG heartbeat = 0;
};
struct Target {
    void **slots = nullptr;
    void *print = nullptr, *printEx = nullptr, *filter = nullptr;
};

SRWLOCK gate = SRWLOCK_INIT;
std::array<Lease, 16> leases;
std::array<Line, capacity> lines;
unsigned begin = 0, count = 0;
std::uint64_t serial = 0, overwritten = 0;
Target installed;
std::atomic<Print> originalPrint{nullptr};
std::atomic<PrintEx> originalPrintEx{nullptr};
std::atomic<Filter> originalFilter{nullptr};
thread_local unsigned forwarding = 0;
std::string lastError;
#ifdef HLSL_STANDALONE_TEST
Target supplied;
std::atomic<ULONGLONG> suppliedTime{0};
#endif

ULONGLONG now() {
#ifdef HLSL_STANDALONE_TEST
    if (auto value = suppliedTime.load())
        return value;
#endif
    return GetTickCount64();
}

// Cleanup/control calls may wait for a short queue copy. Capture never waits,
// never allocates and never calls game/Lua functions while holding this gate.
__declspec(noinline) void withGate(native_hlsl::detail::ProtocolAction action, void *context) {
    AcquireSRWLockExclusive(&gate);
    __try {
        action(context);
    } __finally {
        ReleaseSRWLockExclusive(&gate);
    }
}
template <typename Action> void locked(Action &action) {
    withGate([](void *context) { (*static_cast<Action *>(context))(); }, &action);
}

bool read(const void *address, void *output, size_t bytes) {
    SIZE_T copied = 0;
    return ReadProcessMemory(GetCurrentProcess(), address, output, bytes, &copied) && copied == bytes;
}

bool resolve(Target &target) {
#ifdef HLSL_STANDALONE_TEST
    if (supplied.slots) {
        target = supplied;
        return true;
    }
#endif
    static_assert(sizeof(void *) == 4, "Persistence chat integration requires the supported Win32 client");
    const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"client.dll"));
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS32 pe{};
    if (!module || !read(reinterpret_cast<void *>(module), &dos, sizeof(dos)) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 || dos.e_lfanew > 0x1000 ||
        !read(reinterpret_cast<void *>(module + dos.e_lfanew), &pe, sizeof(pe)) ||
        pe.Signature != IMAGE_NT_SIGNATURE || pe.FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        pe.FileHeader.TimeDateStamp != clientStamp ||
        pe.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        pe.OptionalHeader.SizeOfImage != clientImageSize) {
        lastError = "unsupported or unavailable CS:GO Legacy client image";
        return false;
    }
    void *object = nullptr;
    void **table = nullptr;
    std::array<void *, 37> entries{};
    if (!read(reinterpret_cast<void *>(module + chatPointerRva), &object, sizeof(object)) || !object ||
        !read(object, &table, sizeof(table)) || !table || !read(table, entries.data(), sizeof(entries))) {
        lastError = "native chat object is not ready";
        return false;
    }
    target = {table + 27, reinterpret_cast<void *>(module + printRva),
              reinterpret_cast<void *>(module + printExRva), reinterpret_cast<void *>(module + filterRva)};
    unsigned char first[sizeof(printPrefix)]{}, second[sizeof(printPrefix)]{};
    if (entries[36] != target.filter || !read(target.print, first, sizeof(first)) ||
        !read(target.printEx, second, sizeof(second)) || memcmp(first, printPrefix, sizeof(first)) ||
        memcmp(second, printPrefix, sizeof(second))) {
        lastError = "native chat signatures or filter entry do not match";
        return false;
    }
    return true;
}

bool replace(void **slot, void *expected, void *replacement) {
    return InterlockedCompareExchangePointer(slot, replacement, expected) == expected;
}

void __cdecl print(void *object, int player, int filter, const char *format, ...);
void __cdecl printEx(void *object, int channel, int player, int filter, const char *format, ...);

bool install(const Target &target) {
    void *current[2]{};
    if (!read(target.slots, current, sizeof(current))) {
        lastError = "native chat table is unreadable";
        return false;
    }
    const auto hook = reinterpret_cast<void *>(&print);
    const auto hookEx = reinterpret_cast<void *>(&printEx);
    if (installed.slots == target.slots && current[0] == hook && current[1] == hookEx)
        return true;
    if (installed.slots || current[0] != target.print || current[1] != target.printEx) {
        lastError = "native chat table changed or another hook owns an entry";
        return false;
    }
    DWORD protection = 0;
    if (!VirtualProtect(target.slots, sizeof(current), PAGE_READWRITE, &protection)) {
        lastError = "native chat table protection could not be changed";
        return false;
    }
    originalPrint.store(reinterpret_cast<Print>(target.print));
    originalPrintEx.store(reinterpret_cast<PrintEx>(target.printEx));
    originalFilter.store(reinterpret_cast<Filter>(target.filter));
    const bool first = replace(target.slots, target.print, hook);
    const bool second = first && replace(target.slots + 1, target.printEx, hookEx);
    if (!second && first)
        replace(target.slots, hook, target.print);
    DWORD ignored = 0;
    const bool protectedAgain = VirtualProtect(target.slots, sizeof(current), protection, &ignored) != FALSE;
    if (second)
        installed = target;
    if (!second || !protectedAgain) {
        lastError = !second ? "native chat hook ownership changed during installation"
                            : "native chat table protection could not be restored";
        return false;
    }
    lastError.clear();
    return true;
}

void uninstall() {
    if (!installed.slots)
        return;
    DWORD protection = 0;
    if (!VirtualProtect(installed.slots, sizeof(void *) * 2, PAGE_READWRITE, &protection)) {
        lastError = "native chat table protection could not be restored during cleanup";
        return;
    }
    // Never overwrite another component's replacement. A retained call to this
    // DLL remains a forwarding function after our leases have been removed.
    replace(installed.slots, reinterpret_cast<void *>(&print), installed.print);
    replace(installed.slots + 1, reinterpret_cast<void *>(&printEx), installed.printEx);
    DWORD ignored = 0;
    if (!VirtualProtect(installed.slots, sizeof(void *) * 2, protection, &ignored))
        lastError = "native chat cleanup could not restore table protection";
    installed = {};
}

bool capture(void *object, int player, int filter, const char *text) {
    if (forwarding)
        return false;
    const auto getFilter = originalFilter.load();
    const bool allowed = !filter || !getFilter || (getFilter(object) & unsigned(filter)) != 0;
    bool suppress = false;
    const auto time = now();
    auto copy = [&] {
        bool active = false;
        for (const auto &lease : leases) {
            if (lease.owner && time - lease.heartbeat < leaseMilliseconds) {
                active = true;
                suppress = suppress || lease.hide;
            }
        }
        if (!active || !allowed || !text[0])
            return;
        if (count == capacity) {
            begin = (begin + 1) % capacity;
            --count;
            ++overwritten;
        }
        auto &line = lines[(begin + count++) % capacity];
        line.serial = ++serial;
        line.player = player;
        line.filter = filter;
        memcpy(line.text.data(), text, textCapacity);
    };
    // Contention fails open: the engine prints its original line. It must never
    // block a network/game callback waiting for Lua to poll the queue.
    if (!native_hlsl::detail::try_protocol(gate, copy))
        return false;
    return suppress;
}

void forward(Print function, void *object, int player, int filter, const char *text) {
    if (!function)
        return;
    __try {
        ++forwarding;
        function(object, player, filter, "%s", text);
    } __finally {
        --forwarding;
    }
}
void forward(PrintEx function, void *object, int channel, int player, int filter, const char *text) {
    if (!function)
        return;
    __try {
        ++forwarding;
        function(object, channel, player, filter, "%s", text);
    } __finally {
        --forwarding;
    }
}

void __cdecl print(void *object, int player, int filter, const char *format, ...) {
    char text[textCapacity]{};
    va_list args;
    va_start(args, format);
    if (format)
        _vsnprintf_s(text, sizeof(text), _TRUNCATE, format, args);
    va_end(args);
    if (!capture(object, player, filter, text))
        forward(originalPrint.load(), object, player, filter, text);
}
void __cdecl printEx(void *object, int channel, int player, int filter, const char *format, ...) {
    char text[textCapacity]{};
    va_list args;
    va_start(args, format);
    if (format)
        _vsnprintf_s(text, sizeof(text), _TRUNCATE, format, args);
    va_end(args);
    if (!capture(object, player, filter, text))
        forward(originalPrintEx.load(), object, channel, player, filter, text);
}

void remove(unsigned owner) {
    for (auto &lease : leases)
        if (lease.owner == owner)
            lease = {};
    if (std::none_of(leases.begin(), leases.end(), [](const Lease &lease) { return lease.owner != 0; })) {
        uninstall();
        begin = count = 0;
    }
}
} // namespace

J poll(unsigned owner, bool enabled, bool hide, std::uint64_t since) {
    std::vector<Line> output;
    output.reserve(batchLimit);
    bool active = false, hooked = false;
    std::uint64_t cursor = since, lost = 0;
    std::string error;
    auto update = [&] {
        if (!enabled) {
            remove(owner);
            error = lastError;
            return;
        }
        Target target;
        if (!resolve(target) || !install(target)) {
            remove(owner);
            error = lastError;
            return;
        }
        auto lease = std::find_if(leases.begin(), leases.end(),
                                  [&](const Lease &item) { return item.owner == owner; });
        if (lease == leases.end())
            lease =
                std::find_if(leases.begin(), leases.end(), [](const Lease &item) { return item.owner == 0; });
        if (lease == leases.end()) {
            error = "native chat owner limit";
            return;
        }
        *lease = {owner, hide, now()};
        active = hooked = true;
        for (unsigned index = 0; index < count && output.size() < batchLimit; ++index) {
            const auto &line = lines[(begin + index) % capacity];
            if (line.serial > since) {
                output.push_back(line);
                cursor = line.serial;
            }
        }
        if (!count)
            cursor = serial;
        lost = overwritten;
    };
    // Control JSON/allocations happen outside the capture gate. The vector's
    // capacity is reserved before entering it; queue snapshots only copy bytes.
    locked(update);
    J messages = J::array();
    for (const auto &line : output)
        messages.push_back({{"serial", line.serial},
                            {"player", line.player},
                            {"filter", line.filter},
                            {"text", line.text.data()}});
    return {{"supported", error.empty()},
            {"active", active},
            {"hooked", hooked},
            {"error", error},
            {"cursor", cursor},
            {"overwritten", lost},
            {"messages", std::move(messages)}};
}

void release(unsigned owner) {
    auto action = [&] { remove(owner); };
    locked(action);
}
void shutdown() {
    auto action = [&] {
        leases.fill({});
        uninstall();
        begin = count = 0;
    };
    locked(action);
}
#ifdef HLSL_STANDALONE_TEST
void test_target(void **table, void *first, void *second, void *filter) {
    shutdown();
    supplied = {table, first, second, filter};
    lastError.clear();
}
void test_time(std::uint64_t milliseconds) {
    suppliedTime = milliseconds;
}
#endif
} // namespace hud_chat
