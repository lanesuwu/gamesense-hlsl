// Device hooks and command transport. Accept requests synchronously here;
// execute GPU work through the renderer on the host render thread.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "native_hlsl.h"
#include "dx9_api.h"
#include "protocol_guard.h"
#include "profile9.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

using PresentFn = HRESULT(WINAPI *)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);
using ResetFn = HRESULT(WINAPI *)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
using PresentExFn = HRESULT(WINAPI *)(IDirect3DDevice9Ex *, const RECT *, const RECT *, HWND, const RGNDATA *,
                                      DWORD);
using ResetExFn = HRESULT(WINAPI *)(IDirect3DDevice9Ex *, D3DPRESENT_PARAMETERS *, D3DDISPLAYMODEEX *);
using EndSceneFn = HRESULT(WINAPI *)(IDirect3DDevice9 *);
static std::recursive_mutex stateMutex;
static std::mutex queueMutex;
static IDirect3DDevice9 *device = nullptr;
static IDirect3DDevice9Ex *deviceEx = nullptr;
struct Patch {
    void **slot;
    void *original;
    void *replacement;
};
static std::vector<Patch> patches;
static PresentFn originalPresent = nullptr;
static ResetFn originalReset = nullptr;
static PresentExFn originalPresentEx = nullptr;
static ResetExFn originalResetEx = nullptr;
static EndSceneFn originalEndScene = nullptr;
static std::atomic<UINT> rendered{0}, resets{0};
static std::atomic<UINT> rejectedCommands{0};
static std::atomic<UINT> scenes{0}, presents{0}, duplicateBatches{0};
static std::atomic<bool> deviceReady{false};
static std::atomic<LONG> lastResult{S_OK};
static HRESULT WINAPI presentHook(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);
static HRESULT WINAPI resetHook(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
static HRESULT WINAPI presentExHook(IDirect3DDevice9Ex *, const RECT *, const RECT *, HWND, const RGNDATA *,
                                    DWORD);
static HRESULT WINAPI resetExHook(IDirect3DDevice9Ex *, D3DPRESENT_PARAMETERS *, D3DDISPLAYMODEEX *);
static HRESULT WINAPI endSceneHook(IDirect3DDevice9 *);
static thread_local unsigned presentDepth = 0;
static thread_local unsigned hlslDrawDepth = 0;
// A backbuffer scene completed since the last swap. The draw happens at
// Present, not at EndScene: some hosts (the menu especially) draw after the
// scene ends, and at high frame rates the two orderings alternated, which is
// what flickered. Drawing immediately before the swap is the only position
// with nothing behind it.
static bool sceneReady = false;
#ifdef HLSL_STANDALONE_TEST
static void (*afterRenderTest)(IDirect3DDevice9 *) = nullptr;
#endif

static bool replaceSlot(void **slot, void *expected, void *value) {
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old))
        return false;
    void *before =
        InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile *>(slot), value, expected);
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void *), old, &ignored);
    return before == expected;
}
// DX9 restores its dispatch table when a state block recording ends (including
// D3DX effect creation). Reinstall only slots restored to their known original;
// never chain over an unknown hook, which could already forward back to ours.
static bool refreshHooks() {
    profile9::Scope measured(profile9::HookRepair);
    bool ok = true;
    for (auto &p : patches) {
        auto current = *p.slot;
        if (current == p.replacement)
            continue;
        if (current != p.original || !replaceSlot(p.slot, p.original, p.replacement))
            ok = false;
    }
    if (!ok)
        lastResult = E_ACCESSDENIED;
    return ok;
}
extern "C" {
// Legacy diagnostic marker; the probe reply carries the transport ABI version.
__declspec(dllexport) __declspec(naked) unsigned __cdecl Hlsl_NativeABI() {
    __asm { mov eax, 0x314e474c }
    __asm {mov edx, 0x39305844} __asm {
        ret
    }
}
__declspec(dllexport) int __cdecl Hlsl_Initialize(void *pointer) {
    if (!pointer)
        return E_POINTER;
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    if (device)
        return E_UNEXPECTED;
    auto *d = static_cast<IDirect3DDevice9 *>(pointer);
    D3DCAPS9 caps = {};
    HRESULT hr = d->GetDeviceCaps(&caps);
    if (FAILED(hr))
        return hr;
    if (caps.PixelShaderVersion < D3DPS_VERSION(3, 0))
        return D3DERR_NOTAVAILABLE;
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCSTR>(&Hlsl_Initialize), &pinned))
        return HRESULT_FROM_WIN32(GetLastError());
    auto table = *reinterpret_cast<void ***>(d);
    auto present = reinterpret_cast<PresentFn>(table[17]);
    auto reset = reinterpret_cast<ResetFn>(table[16]);
    if (!present || !reset || present == presentHook || reset == resetHook)
        return E_UNEXPECTED;
    originalPresent = present;
    originalReset = reset;
    originalEndScene = reinterpret_cast<EndSceneFn>(table[42]);
    patches = {{table + 16, reinterpret_cast<void *>(reset), reinterpret_cast<void *>(resetHook)},
               {table + 17, reinterpret_cast<void *>(present), reinterpret_cast<void *>(presentHook)},
               {table + 42, table[42], reinterpret_cast<void *>(endSceneHook)}};
    IDirect3DDevice9Ex *ex = nullptr;
    if (SUCCEEDED(d->QueryInterface(__uuidof(IDirect3DDevice9Ex), reinterpret_cast<void **>(&ex)))) {
        auto exTable = *reinterpret_cast<void ***>(ex);
        if (exTable != table)
            patches.push_back({exTable + 42, exTable[42], reinterpret_cast<void *>(endSceneHook)});
        originalPresentEx = reinterpret_cast<PresentExFn>(exTable[121]);
        originalResetEx = reinterpret_cast<ResetExFn>(exTable[132]);
        patches.push_back({exTable + 121, exTable[121], reinterpret_cast<void *>(presentExHook)});
        patches.push_back({exTable + 132, exTable[132], reinterpret_cast<void *>(resetExHook)});
    }
    size_t installed = 0;
    for (; installed < patches.size(); ++installed) {
        auto &p = patches[installed];
        if (!replaceSlot(p.slot, p.original, p.replacement))
            break;
    }
    if (installed != patches.size()) {
        while (installed) {
            auto &p = patches[--installed];
            replaceSlot(p.slot, p.replacement, p.original);
        }
        patches.clear();
        if (ex)
            ex->Release();
        return E_ACCESSDENIED;
    }
    device = d;
    deviceEx = ex;
    device->AddRef();
    deviceReady = true;
    lastResult = S_OK;
    return S_OK;
}
__declspec(dllexport) int __cdecl Hlsl_Shutdown() {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    if (!device)
        return S_FALSE;
    for (auto &p : patches)
        if (*p.slot != p.replacement && *p.slot != p.original)
            return E_ACCESSDENIED;
    std::vector<Patch *> restored;
    for (auto &p : patches) {
        if (*p.slot == p.original)
            continue;
        if (!replaceSlot(p.slot, p.replacement, p.original)) {
            for (auto it = restored.rbegin(); it != restored.rend(); ++it)
                replaceSlot((*it)->slot, (*it)->original, (*it)->replacement);
            return E_ACCESSDENIED;
        }
        restored.push_back(&p);
    }
    patches.clear();
    dx9_api::shutdown();
    if (deviceEx) {
        deviceEx->Release();
        deviceEx = nullptr;
    }
    device->Release();
    device = nullptr;
    deviceReady = false;
    return S_OK;
}
__declspec(dllexport) void __cdecl Hlsl_Status(unsigned *frames, unsigned *resetCount, int *result) {
    if (frames)
        *frames = rendered.load();
    if (resetCount)
        *resetCount = resets.load();
    if (result)
        *result = lastResult.load();
}
}
static void renderPending(IDirect3DDevice9 *d) {
    if (d == device || (deviceEx && d == static_cast<IDirect3DDevice9 *>(deviceEx))) {
        dx9_api::render(d);
#ifdef HLSL_STANDALONE_TEST
        if (afterRenderTest)
            afterRenderTest(d);
#endif
        ++rendered;
        lastResult = S_OK;
    }
}
static void drawBeforePresent(IDirect3DDevice9 *d) {
    // Not conditional on sceneReady: some menu paths present without a
    // backbuffer scene, and the batch's own freshness plus the per-frame
    // duplicate check already prevent double draws.
    if (hlslDrawDepth)
        return;
    sceneReady = false;
    if (d != device && (!deviceEx || d != static_cast<IDirect3DDevice9 *>(deviceEx)))
        return;
    ++hlslDrawDepth;
    try {
        std::lock_guard<std::recursive_mutex> lock(stateMutex);
        try {
            renderPending(d);
        } catch (...) {
            refreshHooks();
            throw;
        }
        refreshHooks();
    } catch (...) {
        lastResult = E_FAIL;
    }
    --hlslDrawDepth;
}
static HRESULT WINAPI presentHook(IDirect3DDevice9 *d, const RECT *a, const RECT *b, HWND w,
                                  const RGNDATA *dirty) {
    PresentFn original = nullptr;
    ++presentDepth;
    {
        std::lock_guard<std::recursive_mutex> lock(stateMutex);
        original = originalPresent;
    }
    if (presentDepth == 1)
        drawBeforePresent(d);
    HRESULT result = original ? original(d, a, b, w, dirty) : D3DERR_INVALIDCALL;
    if (presentDepth == 1)
        ++presents;
    --presentDepth;
    return result;
}
static HRESULT WINAPI presentExHook(IDirect3DDevice9Ex *d, const RECT *a, const RECT *b, HWND w,
                                    const RGNDATA *dirty, DWORD flags) {
    PresentExFn original = nullptr;
    ++presentDepth;
    {
        std::lock_guard<std::recursive_mutex> lock(stateMutex);
        original = originalPresentEx;
    }
    if (presentDepth == 1)
        drawBeforePresent(static_cast<IDirect3DDevice9 *>(d));
    HRESULT result = original ? original(d, a, b, w, dirty, flags) : D3DERR_INVALIDCALL;
    if (presentDepth == 1)
        ++presents;
    --presentDepth;
    return result;
}
static HRESULT WINAPI resetHook(IDirect3DDevice9 *d, D3DPRESENT_PARAMETERS *p) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    if (d == device) {
        dx9_api::reset();
        sceneReady = false;
        ++resets;
    }
    // All DEFAULT-pool capture resources are released before the original Reset.
    HRESULT hr = originalReset ? originalReset(d, p) : D3DERR_INVALIDCALL;
    lastResult = hr;
    refreshHooks();
    return hr;
}
static HRESULT WINAPI resetExHook(IDirect3DDevice9Ex *d, D3DPRESENT_PARAMETERS *p, D3DDISPLAYMODEEX *mode) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    if (d == deviceEx) {
        dx9_api::reset();
        sceneReady = false;
        ++resets;
    }
    HRESULT hr = originalResetEx ? originalResetEx(d, p, mode) : D3DERR_INVALIDCALL;
    lastResult = hr;
    refreshHooks();
    return hr;
}
static HRESULT WINAPI endSceneHook(IDirect3DDevice9 *d) {
    EndSceneFn original = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(stateMutex);
        auto table = *reinterpret_cast<void ***>(d);
        original = originalEndScene;
        for (const auto &patch : patches)
            if (patch.slot == table + 42 && patch.replacement == reinterpret_cast<void *>(endSceneHook)) {
                original = reinterpret_cast<EndSceneFn>(patch.original);
                break;
            }
    }
    const HRESULT result = original ? original(d) : D3DERR_INVALIDCALL;
    if (FAILED(result) || hlslDrawDepth)
        return result;
    std::lock_guard<std::recursive_mutex> lock(stateMutex);
    if (d != device && (!deviceEx || d != static_cast<IDirect3DDevice9 *>(deviceEx)))
        return result;
    ++scenes;
    // Ignore offscreen passes. Capture only after a completed backbuffer scene,
    // outside BeginScene/EndScene as required by StretchRect.
    IDirect3DSurface9 *target = nullptr, *back = nullptr;
    const bool eligible = SUCCEEDED(d->GetRenderTarget(0, &target)) &&
                          SUCCEEDED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) && target == back;
    if (target)
        target->Release();
    if (back)
        back->Release();
    if (eligible)
        sceneReady = true;
    return result;
}
namespace native_hlsl {
namespace {
SRWLOCK protocolLock = SRWLOCK_INIT;
// Counts abnormal critical-section exits (C++ exceptions and Windows SEH).
volatile LONG protocolUnwinds = 0;
// Diagnostic milestones only: never clear/steal the gate to recover a request.
std::atomic<int> protocolPhase{0}, protocolOwner{0}, protocolSequence{0};
std::atomic<DWORD> protocolStarted{0};
void protocolMark(int phase) {
    protocolPhase.store(phase, std::memory_order_relaxed);
}
void protocolStart(int owner, int sequence) {
    protocolOwner.store(owner, std::memory_order_relaxed);
    protocolSequence.store(sequence, std::memory_order_relaxed);
    protocolStarted.store(GetTickCount(), std::memory_order_relaxed);
    protocolMark(1);
}
bool accessible(const void *p, std::size_t n, bool executable = false) {
    const auto address = reinterpret_cast<std::uintptr_t>(p);
    if (!address || n > UINTPTR_MAX - address)
        return false;
    MEMORY_BASIC_INFORMATION m{};
    if (!VirtualQuery(p, &m, sizeof(m)) || m.State != MEM_COMMIT ||
        (m.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return false;
    auto base = reinterpret_cast<std::uintptr_t>(m.BaseAddress);
    if (address < base || address - base > m.RegionSize || n > m.RegionSize - (address - base))
        return false;
    const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                           PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    const DWORD code = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (m.Protect & (executable ? code : readable)) != 0;
}
bool pointerWord(const void *at, void *&out) {
    if (!accessible(at, 4))
        return false;
    std::memcpy(&out, at, 4);
    return true;
}
// No CRT logger, heap allocation, renderer calls or transaction lock in probes.
struct TextBuffer {
    char data[256]{};
    char *end = data;
    void text(const char *s) {
        const auto n = std::strlen(s);
        if (n <= static_cast<std::size_t>(data + sizeof(data) - end)) {
            std::memcpy(end, s, n);
            end += n;
        }
    }
    template <class T> void number(T value, int base = 10) {
        const auto r = std::to_chars(end, data + sizeof(data), value, base);
        if (r.ec == std::errc{})
            end = r.ptr;
    }
    DWORD size() const {
        return static_cast<DWORD>(end - data);
    }
};
bool write_file(const wchar_t *name, const TextBuffer &text) {
    auto file = CreateFileW(name, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, text.data, text.size(), &written, nullptr) && written == text.size();
    CloseHandle(file);
    return ok;
}
bool probe(int a, int b) {
    if (a <= 0 || a > 32767 || b >= 0 || b < -32767)
        return true;
    TextBuffer text;
    text.text("GS_HLSL_ABI 6 ");
    text.number(a);
    text.text(" ");
    text.number(b);
    text.text("\n");
    write_file(L"gs_hlsl.handshake", text);
    return true;
}
} // namespace

bool discover_device() {
    std::lock_guard<std::recursive_mutex> state(stateMutex);
    if (device)
        return true;
    lastResult = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    // Scan only executable sections in the installed shader module. All matches
    // must agree on one in-module global; don't accept an arbitrary first hit.
    auto module = reinterpret_cast<const BYTE *>(GetModuleHandleW(L"shaderapidx9.dll"));
    if (!accessible(module, sizeof(IMAGE_DOS_HEADER)))
        return false;
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || dos->e_lfanew > 0x100000)
        return false;
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32 *>(module + dos->e_lfanew);
    if (!accessible(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        return false;
    auto sections = IMAGE_FIRST_SECTION(nt);
    if (nt->FileHeader.NumberOfSections > 96 ||
        !accessible(sections, nt->FileHeader.NumberOfSections * sizeof(*sections)))
        return false;
    void **binding = nullptr;
    const auto start = reinterpret_cast<std::uintptr_t>(module);
    const std::uintptr_t end = start + nt->OptionalHeader.SizeOfImage;
    for (unsigned s = 0; s < nt->FileHeader.NumberOfSections; ++s) {
        const auto &section = sections[s];
        if (!(section.Characteristics & IMAGE_SCN_MEM_EXECUTE))
            continue;
        const auto length = section.Misc.VirtualSize;
        if (section.VirtualAddress > nt->OptionalHeader.SizeOfImage ||
            length > nt->OptionalHeader.SizeOfImage - section.VirtualAddress)
            return false;
        auto bytes = module + section.VirtualAddress;
        if (!accessible(bytes, length))
            return false;
        for (unsigned i = 0; i + 11 <= length; ++i) {
            if (bytes[i] != 0xa1 || std::memcmp(bytes + i + 5, "\x50\x8b\x08\xff\x51\x0c", 6))
                continue;
            void *candidate = nullptr;
            std::memcpy(&candidate, bytes + i + 1, 4);
            auto address = reinterpret_cast<std::uintptr_t>(candidate);
            if (address < start || address > end || end - address < 4)
                return false;
            if (binding && binding != candidate)
                return false;
            binding = static_cast<void **>(candidate);
        }
    }
    void *object = nullptr, *table = nullptr;
    if (!binding || !pointerWord(binding, object) || !pointerWord(object, table) ||
        !accessible(table, 119 * 4))
        return false;
    auto entries = static_cast<void **>(table);
    for (auto slot : {0, 1, 2, 3, 16, 17, 42, 106})
        if (!accessible(entries[slot], 1, true))
            return false;
    const auto result = Hlsl_Initialize(object);
    lastResult = result;
    return SUCCEEDED(result);
}

static bool execute(int x, int op, int a, int b) {
    if (x != magic)
        return false;
    if (op == 99)
        return probe(a, b);
    if (op != 80)
        return true; // ABI 4 commands deliberately fail closed.
    if (a < 1 || a > 16777215 || b < 1 || b > 16777215)
        return true;
    using J = dx9_api::json;
    auto name = L"gs_hlsl6_" + std::to_wstring(a);
    J response = {{"sequence", b}, {"ok", false}};
    try {
        auto execute_request = [&] {
            auto file = CreateFileW((name + L".request.json").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
                throw std::runtime_error("request file not readable");
            LARGE_INTEGER size{};
            BOOL sized = GetFileSizeEx(file, &size);
            if (!sized || size.QuadPart < 2 || size.QuadPart > 4 * 1024 * 1024) {
                CloseHandle(file);
                throw std::runtime_error("request size outside 4 MiB limit");
            }
            std::string data(size_t(size.QuadPart), '\0');
            DWORD got = 0;
            BOOL read = ReadFile(file, &data[0], DWORD(data.size()), &got, nullptr);
            CloseHandle(file);
            if (!read || got != data.size())
                throw std::runtime_error("request read failed");
            auto req = J::parse(data, [](int depth, J::parse_event_t, J &) {
                if (depth > 64)
                    throw std::runtime_error("request JSON depth exceeds 64");
                return true;
            });
            if (req.at("sequence").get<int>() != b)
                throw std::runtime_error("stale request sequence");
            J result;
            if (!dx9_api::try_request(unsigned(a), req, result)) {
                response["error"] = "native backend busy; retry next callback";
                return;
            }
            response["result"] = std::move(result);
            response["ok"] = true;
            if (req.at("op") == "status") {
                response["result"]["native_hresult"] = unsigned(lastResult.load());
                response["result"]["device_ready"] = deviceReady.load();
                response["result"]["presents"] = presents.load();
                response["result"]["protocol_unwinds"] = unsigned(protocolUnwinds);
            }
        };
        // Contention is an ordinary rejected request, not a C++ exception.
        if (!detail::try_protocol(protocolLock, execute_request, &protocolUnwinds))
            response["error"] = "native transport busy; retry";
    } catch (const std::exception &e) {
        response["error"] = e.what();
    } catch (...) {
        response["error"] = "native transport exception";
    }
    std::string output;
    {
        profile9::Scope measuredReply(profile9::Reply);
        output = response.dump(-1, ' ', false, J::error_handler_t::replace);
    }
    auto file = CreateFileW((name + L".reply.json").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, output.data(), DWORD(output.size()), &written, nullptr);
        CloseHandle(file);
    }
    // A host-owned state block can also restore the shared DX9 dispatch table.
    // Avoid waiting on the render thread or overwriting another hook.
    {
        std::unique_lock<std::recursive_mutex> state(stateMutex, std::try_to_lock);
        if (state.owns_lock() && device && !hlslDrawDepth)
            refreshHooks();
    }
    // Discovery can call back into the host, so do it only after replying and
    // releasing both protocol and backend locks.
    static ULONGLONG lastAttempt = 0;
    auto now = GetTickCount64();
    if (!deviceReady && now - lastAttempt >= 1000) {
        lastAttempt = now;
        try {
            discover_device();
        } catch (...) {
            lastResult = E_FAIL;
        }
    }
    return true;
}
bool command(int x, int op, int a, int b) {
    try {
        return execute(x, op, a, b);
    } catch (...) {
        lastResult = E_OUTOFMEMORY;
        return x == magic;
    }
}
std::string transport_diagnostics() {
    char text[320]{};
    const auto phase = protocolPhase.load();
    const auto elapsed = DWORD(GetTickCount() - protocolStarted.load());
    std::snprintf(text, sizeof(text),
                  "{\"phase\":%d,\"owner\":%d,\"sequence\":%d,\"elapsed_ms\":%lu,\"protocol_unwinds\":%lu}",
                  phase, protocolOwner.load(), protocolSequence.load(), static_cast<unsigned long>(elapsed),
                  static_cast<unsigned long>(protocolUnwinds));
    return text;
}
std::string memory_request(int owner, int sequence, const char *data, std::size_t size, unsigned format) {
    profile9::Scope measured(profile9::MemoryRequest);
    using J = dx9_api::json;
    // Protected by the same protocol guard as the legacy transport. No Lua
    // allocations/callbacks occur while this lock is held.
    static std::map<unsigned, unsigned> sequences;
    J response = {{"sequence", sequence}, {"ok", false}};
    bool executed = false;
    try {
        auto execute_request = [&] {
            executed = true;
            protocolStart(owner, sequence);
            if (owner < 1 || owner > 16777215 || sequence < 1 || sequence > 16777215)
                throw std::runtime_error("owner/sequence outside API limits");
            if (!data || size < 2 || size > 4u * 1024 * 1024)
                throw std::runtime_error("request size outside 4 MiB limit");
            protocolMark(2); // Decode JSON setup or a compact prepared update.
            J req;
            dx9_api::PreparedUpdate update;
            dx9_api::BufferUpdate buffer;
            std::string op;
            {
                profile9::Scope measuredDecode(profile9::Decode);
                if (format) {
                    const bool prepared = format == 1;
                    const char *cursor = data, *end = data + size;
                    if (format > 2 || size < 3 || memcmp(cursor, prepared ? "P1\n" : "U1\n", 3))
                        throw std::runtime_error("invalid compact update header");
                    cursor += 3;
                    auto line = [&]() {
                        const char *start = cursor;
                        while (cursor != end && *cursor != '\n')
                            ++cursor;
                        if (cursor == end || cursor == start || cursor - start > 48)
                            throw std::runtime_error("invalid compact update line");
                        const char *stop = cursor++;
                        return std::make_pair(start, stop);
                    };
                    auto integer = [&]() {
                        const auto text = line();
                        unsigned value = 0;
                        const auto parsed = std::from_chars(text.first, text.second, value);
                        if (parsed.ec != std::errc() || parsed.ptr != text.second)
                            throw std::runtime_error("invalid compact update integer");
                        return value;
                    };
                    if (!prepared) {
                        buffer.handle = integer();
                        buffer.offset = integer();
                        if (buffer.handle > 16777215 || buffer.offset > 64 * 1024 * 1024)
                            throw std::runtime_error("binary update outside limits");
                        buffer.bytes = cursor;
                        buffer.size = size_t(end - cursor);
                        op = "update_raw";
                    } else {
                        update.slot = integer();
                        update.retain = integer();
                        const auto count = integer();
                        if (update.slot < 1 || update.slot > 2 || update.retain > 250 || count > 65536)
                            throw std::runtime_error("prepared update outside limits");
                        update.values.reserve(count);
                        for (unsigned i = 0; i < count; ++i) {
                            const auto text = line();
                            double value = 0;
                            const auto parsed =
                                std::from_chars(text.first, text.second, value, std::chars_format::general);
                            if (parsed.ec != std::errc() || parsed.ptr != text.second ||
                                !std::isfinite(value) || std::abs(value) > 3.4028234663852886e38)
                                throw std::runtime_error("invalid prepared float32 constant");
                            update.values.push_back(float(value));
                        }
                        if (cursor != end)
                            throw std::runtime_error("prepared update has trailing bytes");
                        op = "submit_prepared";
                    }
                } else {
                    req = J::parse(data, data + size, [](int depth, J::parse_event_t, J &) {
                        if (depth > 64)
                            throw std::runtime_error("request JSON depth exceeds 64");
                        return true;
                    });
                    if (!req.at("sequence").is_number_integer() ||
                        req.at("sequence").get<int64_t>() != sequence)
                        throw std::runtime_error("stale request sequence");
                    op = req.at("op").get<std::string>();
                }
            }
            protocolMark(3); // decoded; sequence/owner validation
            const auto previous = sequences.find(unsigned(owner));
            if (op == "open" && previous == sequences.end() && sequences.size() >= 16)
                throw std::runtime_error("memory transport owner limit reached");
            if (op != "open" && (previous == sequences.end() || unsigned(sequence) <= previous->second))
                throw std::runtime_error("unknown owner or replayed memory request");
            protocolMark(4); // backend call
            J result;
            const bool accepted = format == 2
                                      ? dx9_api::try_update(unsigned(owner), buffer, result)
                                      : (format == 1 ? dx9_api::try_submit(unsigned(owner), update, result)
                                                     : dx9_api::try_request(unsigned(owner), req, result));
            if (!accepted) {
                response["error"] = "native backend busy; retry next callback";
                return;
            }
            response["result"] = std::move(result);
            protocolMark(5); // backend returned; reply fields
            response["ok"] = true;
            if (op == "close")
                sequences.erase(unsigned(owner));
            else
                sequences[unsigned(owner)] = unsigned(sequence);
            protocolMark(6); // sequence recorded
            if (op == "status") {
                response["result"]["native_hresult"] = unsigned(lastResult.load());
                response["result"]["device_ready"] = deviceReady.load();
                response["result"]["presents"] = presents.load();
                response["result"]["protocol_unwinds"] = unsigned(protocolUnwinds);
                response["result"]["transport"] = "memory";
            }
            protocolMark(7); // all request work completed; about to release gate
        };
        // Contention is an ordinary rejected request, not a C++ exception.
        if (!detail::try_protocol(protocolLock, execute_request, &protocolUnwinds))
            response["error"] = "native transport busy; retry";
        if (executed)
            protocolMark(8); // gate released
    } catch (const std::exception &e) {
        response["error"] = e.what();
    } catch (...) {
        response["error"] = "native memory transport exception";
    }
    // Preserve the interrupted request's milestone when rejecting contention.
    const bool diagnostic_owner =
        executed && protocolOwner.load() == owner && protocolSequence.load() == sequence;
    if (diagnostic_owner)
        protocolMark(9); // JSON reply serialization
    std::string output;
    {
        profile9::Scope measuredReply(profile9::Reply);
        output = response.dump(-1, ' ', false, J::error_handler_t::replace);
    }
    if (diagnostic_owner)
        protocolMark(10); // native hook refresh/discovery
    {
        std::unique_lock<std::recursive_mutex> state(stateMutex, std::try_to_lock);
        if (state.owns_lock() && device && !hlslDrawDepth)
            refreshHooks();
    }
    static ULONGLONG lastAttempt = 0;
    const auto now = GetTickCount64();
    if (!deviceReady && now - lastAttempt >= 1000) {
        lastAttempt = now;
        try {
            discover_device();
        } catch (...) {
            lastResult = E_FAIL;
        }
    }
    if (diagnostic_owner)
        protocolMark(11); // ready to return to Lua adapter
    return output;
}
#ifdef HLSL_STANDALONE_TEST
void test_hold_protocol() {
    AcquireSRWLockExclusive(&protocolLock);
}
void test_after_render(void (*callback)(IDirect3DDevice9 *)) {
    afterRenderTest = callback;
}
void test_release_protocol() {
    ReleaseSRWLockExclusive(&protocolLock);
}
#endif
} // namespace native_hlsl
