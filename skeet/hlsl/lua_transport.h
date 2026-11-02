#pragma once
#include <cstddef>
#include <cstdint>

namespace lua_transport {
// Supplied embedded LuaJIT image only. Installs before its checksum preparation.
bool install(std::uint8_t *image, std::size_t size);
#ifdef HLSL_STANDALONE_TEST
using CFunction = int(__cdecl *)(void *);
using GetTop = int(__cdecl *)(void *);
using GetArg = const void *(__cdecl *)(void *, int);
using PushString = void(__cdecl *)(void *, const char *, std::size_t);
void configure_test(GetTop, GetArg, PushString, CFunction);
int invoke_test(void *);
#endif
} // namespace lua_transport
