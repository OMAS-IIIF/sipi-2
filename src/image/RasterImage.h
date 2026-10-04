#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sipi {
/** Supported packed channel layouts. RGBA always stores straight alpha. */
enum class PixelLayout { rgb, rgba, gray };

/** Owning 8-bit raster: top-left, row-major, gray (0 black, 255 white), interleaved RGB or straight RGBA,
 * without row padding. Exactly width*height*channel_count() bytes are required.
 * Gray and RGB retain the source encoding; no layout implies linear light or
 * sRGB. Alpha is coverage (0 transparent, 255 opaque), never gamma-corrected.
 * Decoders normalize associated TIFF alpha to straight RGBA; hidden RGB values
 * may exist under zero alpha. Filtering must alpha-weight color contributions.
 * ICC bytes are owned and unchanged, without profile validation/conversion.
 * No file handle, codec pointer or reference survives decoding. */
struct RasterImage8 {
    std::uint32_t width, height;
    std::vector<std::uint8_t> pixels;
    std::vector<std::uint8_t> icc_profile;
    PixelLayout layout = PixelLayout::rgb;

    /** Zero denotes an invalid manually constructed layout. */
    [[nodiscard]] std::size_t channel_count() const noexcept {
        switch (layout) {
        case PixelLayout::gray: return 1;
        case PixelLayout::rgb: return 3;
        case PixelLayout::rgba: return 4;
        }
        return 0;
    }
};
} // namespace sipi
