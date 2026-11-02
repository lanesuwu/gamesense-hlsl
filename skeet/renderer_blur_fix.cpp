#include "pch.h"
#include "renderer_blur_fix.h"
#include "hlsl/native_hlsl.h"
#include "hlsl/lua_transport.h"
#include <cstring>

namespace renderer_blur_fix {
namespace {
    constexpr std::uintptr_t imageBase = 0x43310000;
    constexpr std::size_t producerOffset = 0xE2AE2;
    constexpr std::size_t limitOffset = 0xE2AF1;
    constexpr std::size_t callOffset = 0xE3DB6;
    constexpr std::size_t engineOffset = 0x15A514;
    constexpr std::uintptr_t blurVtable = 0x434722B8;
    using Blur = void(__thiscall*)(void*, int, int, int, int);
    using IsInGame = bool(__thiscall*)(void*);
    std::uint8_t* mappedImage = nullptr;

    bool accessible(const void* pointer, std::size_t length, bool write = false) {
        const auto address = reinterpret_cast<std::uintptr_t>(pointer);
        if (!address || length > UINTPTR_MAX - address) return false;
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(pointer, &region, sizeof(region)) || region.State != MEM_COMMIT
            || (region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        const auto regionBase = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
        if (address < regionBase || length > region.RegionSize
            || address - regionBase > region.RegionSize - length) return false;
        const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY
            | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY
            | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        return (region.Protect & (write ? writable : readable)) != 0;
    }

    std::uintptr_t word(const void* pointer) {
        std::uintptr_t value;
        std::memcpy(&value, pointer, sizeof(value));
        return value;
    }

    bool validQueue(std::uint8_t* context) {
        if (!accessible(context, 0x34, true) || word(context + 8) != blurVtable) return false;
        const auto first = word(context + 0x28);
        const auto last = word(context + 0x2C);
        const auto capacity = word(context + 0x30);
        if (!first) return last == 0 && capacity == 0;
        return first >= 0x10000 && first % 4 == 0 && last >= first && capacity >= last
            && (last - first) % 8 == 0 && (capacity - first) % 8 == 0
            && capacity - first <= 0x100000;
    }

    bool inGame() {
        auto engine = reinterpret_cast<void*>(word(mappedImage + engineOffset));
        if (!accessible(engine, sizeof(void*))) return false;
        auto table = reinterpret_cast<std::uintptr_t*>(word(engine));
        // IEngineClient::IsInGame, also exposed by the bundled SDK at slot 26.
        if (!accessible(table, 27 * sizeof(void*))) return false;
        auto function = reinterpret_cast<IsInGame>(table[26]);
        MEMORY_BASIC_INFORMATION region{};
        if (!function || !VirtualQuery(reinterpret_cast<void*>(function), &region, sizeof(region))
            || region.State != MEM_COMMIT || (region.Protect & (PAGE_GUARD | PAGE_NOACCESS))
            || !(region.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ
                | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return false;
        return function(engine);
    }

    // Match the original thiscall's ECX and four stack arguments. EDX is unused.
    void __fastcall blur(void* self, void*, int x, int y, int width, int height) {
        if (native_hlsl::command(x,y,width,height)) return;
        auto context = static_cast<std::uint8_t*>(self);
        if (!validQueue(context)) {
            static bool reported = false;
            if (!reported) OutputDebugStringA("[blur fix] Invalid controller/vector; request ignored.\n");
            reported = true;
            return;
        }
        if (!inGame()) {
            // Same end=begin clear as the native consumer; keep allocation/capacity.
            std::memcpy(context + 0x2C, context + 0x28, sizeof(std::uint32_t));
            return;
        }
        reinterpret_cast<Blur>(mappedImage + producerOffset)(self, x, y, width, height);
    }

    template<std::size_t N>
    bool matches(const std::uint8_t* image, std::size_t size, std::size_t offset, const char (&bytes)[N]) {
        return offset <= size && N - 1 <= size - offset
            && std::memcmp(image + offset, bytes, N - 1) == 0;
    }
}

bool install(std::uint8_t* image, std::size_t size) {
    static_assert(sizeof(void*) == 4, "The embedded image and blur fix require Win32.");
    if (mappedImage) return mappedImage == image; // Installed once before entry.
    if (reinterpret_cast<std::uintptr_t>(image) != imageBase || size < engineOffset + sizeof(void*)
        || !accessible(image, size, true)
        || !matches(image,size,producerOffset,"\x56\x8D\x71\x28\x8B\x4E\x04\x8B\xC1\x2B\x06\xC1\xF8\x03\x3D\x00\x20\x00\x00\x73\x45")
        || !matches(image,size,0xE3DA5,"\x8B\x0D\x20\x5A\x47\x43\x83\xC4\x20\x85\xC9\x74\x09\x50\x55\x53\x57\xE8")
        || !matches(image,size,callOffset,"\xE8\x27\xED\xFF\xFF")
        || !matches(image,size,0xB88B6,"\x8B\x46\x20\x89\x46\x24\x5F\x5E\xC2\x0C\x00")
        || !matches(image,size,0x1622B8,"\x96\x88\x3C\x43")
        || !matches(image,size,0xB03C0,"\x8B\x45\x08\x8B\xCA\xC1\xE1\x02\x89\x08\x6B\xCA\x06")
        || !matches(image,size,0x16C9D,"\x8B\x0D\x14\xA5\x46\x43\x8D\x94\x24\xB0\x02\x00\x00\x52\x57")) {
        OutputDebugStringA("[blur fix] Build/patch validation failed; no blur changes applied.\n");
        return false;
    }
    const auto displacement = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&blur)
        - reinterpret_cast<std::uintptr_t>(image + callOffset + 5));
    const auto oldDisplacement = word(image + callOffset + 1);
    mappedImage = image;
    image[limitOffset + 1] = 0x10; // 4096 rectangles, 24576 indices, all native producers.
    std::memcpy(image + callOffset + 1, &displacement, sizeof(displacement));
    if (!FlushInstructionCache(GetCurrentProcess(), image, size)) {
        std::memcpy(image + callOffset + 1, &oldDisplacement, sizeof(std::uint32_t));
        image[limitOffset + 1] = 0x20;
        FlushInstructionCache(GetCurrentProcess(), image, size);
        mappedImage = nullptr;
        return false;
    }
    OutputDebugStringA("[blur fix] Built-in Lua blur guard active; native limit 4096 rectangles.\n");
    OutputDebugStringA(lua_transport::install(image,size)
        ? "[HLSL] Memory Lua transport active.\n" : "[HLSL] Memory bridge validation failed; file transport retained.\n");
    return true;
}
}
