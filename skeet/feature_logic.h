#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace feature_logic {
inline float throw_pitch(float pitch) { return pitch - 10.f + std::fabs(pitch) / 9.f; }
inline float inverse_throw_pitch(float pitch) {
    return pitch > -10.f ? .9f * pitch + 9.f : 1.125f * pitch + 11.25f;
}
inline float progress(double elapsed, int milliseconds) {
    return static_cast<float>(std::clamp(elapsed * 1000. / std::max(milliseconds, 1), 0., 1.));
}
inline unsigned blend(unsigned from, unsigned to, float amount) {
    return static_cast<unsigned>(std::lround(from + (static_cast<double>(to) - from) * amount));
}
inline unsigned scale_alpha(unsigned color, unsigned alpha) {
    return (color & 0xffffffu) | ((((color >> 24) * std::min(alpha, 255u) + 127u) / 255u) << 24);
}
inline float cooldown_seconds(bool enabled, int milliseconds) {
    return enabled ? std::clamp(milliseconds, 0, 500) * .001f : .5f;
}
}
