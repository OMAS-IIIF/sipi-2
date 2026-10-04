#pragma once
#include <cstdint>

namespace sipi {
/** Exact interval in pixel-edge coordinates relative to an owning raster.
 * Offset and extent share a positive denominator; fractions need not be reduced.
 * Extent must be positive and the entire interval must lie inside the raster. */
struct PixelInterval {
    std::uint64_t offset_numerator;
    std::uint64_t extent_numerator;
    std::uint32_t denominator;
};

/** Fractional crop in normalized raster coordinates, independent of file format.
 * Used when a full-resolution crop maps between pixels of a reduced image. */
struct PixelWindow {
    PixelInterval horizontal;
    PixelInterval vertical;
};
} // namespace sipi
