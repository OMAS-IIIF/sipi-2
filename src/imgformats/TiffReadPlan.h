#pragma once

#include "TiffMetadata.h"
#include "image/PixelWindow.h"
#include "iiifparser/IIIFGeometry.h"

namespace sipi {

/** Owning geometric plan, valid only for the source version used for metadata.
 * All coordinates are orientation-normalized, NOT raw stored TIFF coordinates.
 * A future decoder must normalize orientation or transform the read rectangle.
 * read_region encloses the exact window (horizontal/vertical) in directory.
 * This does not describe tiles, codec support, interpolation halos or I/O calls. */
struct TiffReadPlan {
    TiffDirectory directory;
    PixelRegion read_region;
    PixelInterval horizontal;
    PixelInterval vertical;
};

/** Pure, thread-safe planning from metadata and already resolved IIIF geometry.
 * Metadata levels must satisfy read_tiff_metadata's compatibility contract.
 * Select the smallest-area level whose exact mapped crop covers the output
 * dimensions in BOTH axes. Ties retain metadata order. Otherwise use the base
 * image, including when output requires enlargement; policy/codec checks remain
 * the caller's responsibility. No input mutation, pixel allocation or I/O.
 * Throws std::invalid_argument for invalid dimensions, orientation, level bounds
 * or geometry. Returned fractional edges must be respected when resampling:
 * scaling the entire integer rectangle would change the requested field of view.
 */
[[nodiscard]] TiffReadPlan plan_tiff_read(const TiffMetadata& metadata,
                                         const iiif::ImageGeometry& geometry);

} // namespace sipi
