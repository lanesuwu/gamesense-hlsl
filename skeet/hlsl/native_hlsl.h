#pragma once
#include "hlsl_renderer.h"
#include <string>

// Reserved renderer.blur transport, version 6. Ordinary requests return false.
namespace native_hlsl {
constexpr int magic = -1279741441;
bool command(int x, int operation, int a, int b);
bool discover_device();
std::string transport_diagnostics();
std::string memory_request(int owner, int sequence, const char *data, std::size_t size, unsigned format = 0);
#ifdef HLSL_STANDALONE_TEST
void test_hold_protocol();
void test_release_protocol();
void test_after_render(void (*callback)(IDirect3DDevice9 *));
#endif
} // namespace native_hlsl
extern "C" {
__declspec(dllexport) int __cdecl Hlsl_Initialize(void *);
__declspec(dllexport) int __cdecl Hlsl_Shutdown();
__declspec(dllexport) void __cdecl Hlsl_Status(unsigned *, unsigned *, int *);
}
