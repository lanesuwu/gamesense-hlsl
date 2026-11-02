#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace numeric_helpers {
// VM snapshots use 64-bit storage, but this loader executes x86 instructions.
// Register writes and VM arithmetic deliberately retain the low 32 bits.
constexpr std::uint32_t x86_word(std::uint64_t value) noexcept {
    return static_cast<std::uint32_t>(value);
}

inline int truncate_int(float value) {
    const double wide = value;
    if (!std::isfinite(wide) || wide < (std::numeric_limits<int>::min)() ||
        wide > (std::numeric_limits<int>::max)())
        throw std::out_of_range("Integer vector component is outside int range");
    return static_cast<int>(value);
}

inline void check_listbox_count(std::size_t count) {
    // SSpecs stores the terminal count as well as individual indices.
    if (count > static_cast<std::size_t>((std::numeric_limits<short>::max)()))
        throw std::length_error("Listbox count exceeds its native short index range");
}
} // namespace numeric_helpers
