#pragma once
#include <cstddef>
#include <cstdint>

namespace renderer_blur_fix {
    // Install before starting the embedded image. The loader DLL must stay loaded.
    bool install(std::uint8_t* image, std::size_t size);
}
