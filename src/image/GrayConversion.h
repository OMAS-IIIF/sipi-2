#pragma once
#include "RasterImage.h"
#include <stdexcept>

namespace sipi {
class ColorConversionError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
/** Consume an owning raster and convert RGB/RGBA to Gray Gamma 2.2, D50 using
 * LittleCMS. Existing gray/gray-alpha is returned unchanged (no profile assignment).
 * Use embedded RGB ICC first; otherwise explicit sRGB or, only for an untagged
 * raster, assume sRGB. Unsupported source declarations must be rejected by readers.
 * Invalid/non-RGB/unusable embedded ICC fails: never fall back to sRGB on error.
 * Fixed relative-colorimetric intent with black-point compensation; alpha copied
 * unchanged. Return gray/gray-alpha with deterministic target ICC and no sRGB flag.
 * No compositing, gamma-space filtering or changes to the original repository file.
 * Independent calls own LCMS contexts/profiles/transforms; no global error handler.
 * Budget bounds input+output+source/target ICC, not native LCMS scratch/allocator
 * overhead. Errors throw ColorConversionError; allocation failures may propagate.
 */
[[nodiscard]] RasterImage8 convert_to_gray(RasterImage8 image,
    std::size_t max_working_bytes = 256 * 1024 * 1024);
}
