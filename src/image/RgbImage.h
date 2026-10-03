#pragma once

#include <cstdint>
#include <vector>

namespace sipi {

/** Owning 8-bit RGB raster: top-left origin, row-major, interleaved RGB, no row
 * padding or alpha. A successful decoder returns exactly width*height*3 bytes.
 * Samples retain the source color space; RGB does not imply sRGB. ICC bytes are
 * owned and unchanged (empty when absent), without profile validation/conversion.
 * No file handle, codec pointer, or reference to input storage survives decoding. */
struct RgbImage8 {
    std::uint32_t width, height;
    std::vector<std::uint8_t> pixels;
    std::vector<std::uint8_t> icc_profile;
};

} // namespace sipi
