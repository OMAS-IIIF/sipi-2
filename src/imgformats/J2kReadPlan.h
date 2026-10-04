#pragma once
#include "J2kReader.h"
#include "image/PixelWindow.h"
#include "iiifparser/IIIFGeometry.h"

namespace sipi {
/** A codec resolution and enclosing rectangle in that resolution's raster.
 * source_region is relative to the full image origin, for Grok's window API.
 * window retains exact fractional crop edges inside the decoded rectangle.
 * The plan belongs to the same source version as its metadata. */
struct J2kReadPlan {
    std::uint8_t reduction;
    PixelRegion source_region;
    PixelRegion reduced_region;
    PixelWindow window;
};

/** Select the coarsest main-header resolution sufficient in both axes.
 * Reduced extents use ceil(grid_end/2^r)-ceil(grid_start/2^r); each resolution
 * represents the complete image, as with TIFF pyramids. Map the requested field
 * of view proportionally into that raster, retaining rational crop edges.
 * Tile coding overrides are checked by the codec when reading; no silent retry.
 * Invalid geometry throws std::invalid_argument. No I/O or pixel allocation. */
[[nodiscard]] J2kReadPlan plan_j2k_read(const J2kMetadata& metadata,
                                      const iiif::ImageGeometry& geometry);
} // namespace sipi
