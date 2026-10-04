#pragma once

#include "TiffMetadata.h"
#include "image/PixelRegion.h"
#include "image/RgbImage.h"

#include <cstddef>

namespace sipi {

/** Decode a crop from the first TIFF image at full stored resolution.
 * Supports tiled, unsigned 8-bit, contiguous RGB with exactly three samples,
 * top-left orientation, and no extra samples. No scaling, color conversion,
 * pyramid selection, or HTTP I/O. Unsupported layouts throw TiffUnsupportedError
 * (a TiffReadError subtype); corrupt pixel data throws TiffReadError.
 *
 * The crop must already fit the image; this function validates but never clips.
 * It decodes only tiles intersecting the crop, using one reusable tile buffer.
 * max_decoded_bytes bounds output plus that buffer (default 256 MiB); metadata
 * and libtiff's own allocations retain the separate metadata reader limits.
 * Short/corrupt tile reads fail without exposing partial results. Allocation
 * failures may propagate. Concurrent calls own independent TIFF handles and cursors.
 *
 * Borrows an already opened ByteSource; no path is opened or reopened. Tags are
 * checked through a fresh TIFF handle/cursor against that same source version.
 * Reuse the source used for metadata planning. ByteSourceError propagates for I/O
 * failures. Local atomic replacement is safe; in-place file writes are excluded.
 */
[[nodiscard]] RgbImage8 read_tiff_region(const ByteSource& source,
                                        const PixelRegion& region,
                                        std::size_t max_decoded_bytes = 256 * 1024 * 1024);

} // namespace sipi
