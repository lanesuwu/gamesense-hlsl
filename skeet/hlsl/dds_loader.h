#pragma once
#include <Windows.h>
#include <cstddef>
#include <d3d9.h>
#include <vector>

// Minimal DDS decoder for user textures. Uncompressed 32-bit and DXT1/DXT5.
// Returns BGRA32 pixels; the caller creates the texture.
namespace dds {

struct Image {
    unsigned width = 0, height = 0;
    std::vector<unsigned> pixels;
};

// Read and decode. `why` receives a reason on failure.
bool decode(const wchar_t *path, Image &out, char *why, unsigned cap);

} // namespace dds
