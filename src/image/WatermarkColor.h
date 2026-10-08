#pragma once
#include "GrayConversion.h"
#include <array>

namespace sipi {
/** Convert a single configured sRGB8 color into the destination raster encoding.
 * Return one gray or three RGB bytes, ready for apply_watermark; alpha is handled
 * separately by the compositor. Borrow and validate the raster without changing
 * pixels, profile or declarations. Embedded matching RGB/GRAY ICC takes precedence;
 * invalid/unusable profiles fail, never silently fall back. With no ICC, explicit
 * sRGB uses its RGB/gray encoding; untagged RGB assumes sRGB, untagged gray assumes
 * Gray Gamma 2.2/D50 ONLY for this conversion (no profile is assigned to the image).
 * LittleCMS uses relative colorimetry with black-point compensation. All native
 * state is per call, owned by RAII; independent calls are thread-safe. Only one
 * color is transformed, never the image. ColorConversionError on invalid input or
 * transform failure; allocation failures may propagate. Budget validates borrowed
 * raster/profile bytes, not native LCMS scratch. ICC is additionally capped at 16 MiB.
 */
[[nodiscard]] std::vector<std::uint8_t> watermark_color(
    const RasterImage8& destination, const std::array<std::uint8_t,3>& srgb,
    std::size_t max_working_bytes = 256 * 1024 * 1024);
}
