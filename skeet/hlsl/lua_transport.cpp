// Embedded LuaJIT adapter only. Protocol validation and resource ownership live
// in native_hlsl/dx9_api; ordinary blur calls must retain the original path.
#include "lua_transport.h"
#include "native_hlsl.h"
#include <Windows.h>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <string>

namespace lua_transport {
namespace {
using CFunction = int(__cdecl *)(void *);
using GetTop = int(__cdecl *)(void *);
struct Value {
    std::uint32_t low, tag;
};
using IndexFunction = const Value *(__cdecl *)(void *, int);
using PushString = void(__cdecl *)(void *, const char *, std::size_t);
GetTop getTop = nullptr;
IndexFunction getArg = nullptr;
PushString pushString = nullptr;
CFunction original = nullptr;
std::uint8_t *installedImage = nullptr;
thread_local std::string reply;
thread_local bool pushing = false;
constexpr std::uintptr_t base = 0x43310000;
constexpr std::size_t entry = 0xe3d5d;
constexpr char capability[] = "GS_HLSL_MEMORY_1";

bool integer(const Value *v, int &out) {
    if (!v || v->tag >= 0xfffffff2u)
        return false;
    double n;
    std::memcpy(&n, v, sizeof(n));
    if (!std::isfinite(n) || n < -2147483648. || n > 2147483647. || std::floor(n) != n)
        return false;
    out = static_cast<int>(n);
    return true;
}
// No native locks or automatic C++ objects survive this Lua allocation boundary.
// __finally restores the guard if LuaJIT unwinds through Windows SEH.
int push(void *state, const char *data, std::size_t size) {
    const bool previous = pushing;
    __try {
        pushing = true;
        pushString(state, data, size);
    } __finally {
        pushing = previous;
    }
    return 1;
}
int __cdecl dispatch(void *state) {
    int magic = 0, op = 0;
    if (getTop(state) < 2 || !integer(getArg(state, 1), magic) || magic != native_hlsl::magic ||
        !integer(getArg(state, 2), op) || (op != 81 && op != 82 && op != 83 && op != 84 && op != 85))
        return original(state);
    // Capability text is immutable, so probing during allocation is safe. A
    // missing probe reply would otherwise make auto transport choose slow files.
    if (op == 81)
        return push(state, capability, sizeof(capability) - 1);
    if (pushing) {
        int owner = 0, sequence = 0;
        if (getTop(state) != 5 || !integer(getArg(state, 3), owner) || !integer(getArg(state, 4), sequence))
            return 0;
        // Stack storage belongs to this nested call and cannot invalidate the
        // outer reply string. push() preserves the outer allocation guard.
        char busy[160]{};
        const auto length = std::snprintf(
            busy, sizeof(busy), "{\"sequence\":%d,\"ok\":false,\"error\":\"native transport busy; retry\"}",
            sequence);
        if (length < 0 || size_t(length) >= sizeof(busy))
            return 0;
        return push(state, busy, size_t(length));
    }
    if (op == 83) {
        reply = native_hlsl::transport_diagnostics();
        return push(state, reply.data(), reply.size());
    }
    int owner = 0, sequence = 0;
    if (getTop(state) != 5 || !integer(getArg(state, 3), owner) || !integer(getArg(state, 4), sequence))
        return 0;
    const Value *value = getArg(state, 5);
    if (!value || value->tag != 0xfffffffbu)
        return 0;
    const auto *string = reinterpret_cast<const std::uint8_t *>(value->low);
    std::uint32_t length = 0;
    std::memcpy(&length, string + 12, sizeof(length));
    if (length > 4u * 1024 * 1024)
        return 0;
    try {
        reply = native_hlsl::memory_request(owner, sequence, reinterpret_cast<const char *>(string + 16),
                                            length, op == 85 ? 2u : (op == 84 ? 1u : 0u));
    } catch (...) {
        return 0;
    }
    return push(state, reply.data(), reply.size());
}
template <std::size_t N>
bool bytes(const std::uint8_t *image, std::size_t size, std::size_t off, const char (&s)[N]) {
    return off <= size && N - 1 <= size - off && std::memcmp(image + off, s, N - 1) == 0;
}
} // namespace

bool install(std::uint8_t *image, std::size_t size) {
    static_assert(sizeof(void *) == 4, "Embedded LuaJIT bridge requires Win32");
    if (installedImage)
        return installedImage == image;
    if (reinterpret_cast<std::uintptr_t>(image) != base ||
        !bytes(image, size, entry, "\x56\x8b\x74\x24\x08\x56\xe8\x09\x0a\x04\x00") ||
        !bytes(image, size, 0x124771, "\x8b\x4c\x24\x04\x8b\x41\x14\x2b\x41\x10\xc1\xf8\x03\xc3") ||
        !bytes(image, size, 0x128467, "\x55\x8b\xec\x8b\x4d\x0c\x85\xc9\x7e\x1f") ||
        !bytes(image, size, 0x116891, "\x56\x8b\x74\x24\x08\x8b\x4e\x08\x8b\x41\x14\x3b\x41\x18\x72\x07"))
        return false;
    auto *trampoline =
        static_cast<std::uint8_t *>(VirtualAlloc(nullptr, 10, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!trampoline)
        return false;
    std::memcpy(trampoline, image + entry, 5);
    trampoline[5] = 0xe9;
    const auto back = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(image + entry + 5) -
                                                 reinterpret_cast<std::uintptr_t>(trampoline + 10));
    std::memcpy(trampoline + 6, &back, 4);
    DWORD old = 0;
    if (!VirtualProtect(trampoline, 10, PAGE_EXECUTE_READ, &old) ||
        !FlushInstructionCache(GetCurrentProcess(), trampoline, 10)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    getTop = reinterpret_cast<GetTop>(image + 0x124771);
    getArg = reinterpret_cast<IndexFunction>(image + 0x128467);
    pushString = reinterpret_cast<PushString>(image + 0x116891);
    original = reinterpret_cast<CFunction>(trampoline);
    std::uint8_t patch[5] = {0xe9};
    const auto jump = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&dispatch) -
                                                 reinterpret_cast<std::uintptr_t>(image + entry + 5));
    std::memcpy(patch + 1, &jump, 4);
    if (!VirtualProtect(image + entry, 5, PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    std::memcpy(image + entry, patch, 5);
    const bool flushed = FlushInstructionCache(GetCurrentProcess(), image + entry, 5) != FALSE;
    if (!flushed) {
        std::memcpy(image + entry, trampoline, 5);
        FlushInstructionCache(GetCurrentProcess(), image + entry, 5);
    }
    DWORD ignored;
    VirtualProtect(image + entry, 5, old, &ignored);
    if (!flushed) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    installedImage = image;
    return true;
}
#ifdef HLSL_STANDALONE_TEST
void configure_test(GetTop top, const void *(__cdecl *arg)(void *, int), PushString put, CFunction fallback) {
    getTop = top;
    getArg = reinterpret_cast<IndexFunction>(arg);
    pushString = put;
    original = fallback;
}
int invoke_test(void *state) {
    return dispatch(state);
}
#endif
} // namespace lua_transport
