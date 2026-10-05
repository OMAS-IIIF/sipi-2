#pragma once
#include "RasterImage.h"

namespace sipi {
/** Return an owning, channel-matching ICC v4 profile for explicitly declared sRGB.
 * Gray uses the same sRGB transfer curve/white point; RGB/RGBA use sRGB primaries.
 * Intent 0..3 is preserved in the ICC header; the optional profile ID is cleared.
 * This assigns a description to already-sRGB samples; it never transforms pixels.
 * Embedded CC0 profiles require no runtime files, global mutation or color engine.
 * Invalid layout/intent throws invalid_argument. See third_party/compact-icc. */
[[nodiscard]] std::vector<std::uint8_t> srgb_profile(PixelLayout layout, unsigned intent);
}
