#pragma once
#include <cstddef>
#include <cstdint>

namespace native_additions {
// Install once, before the embedded image's startup checksum is recorded.
bool install(std::uint8_t* image, std::size_t size);
}
