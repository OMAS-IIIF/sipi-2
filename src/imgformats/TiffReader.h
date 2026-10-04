#pragma once

#include "TiffMetadata.h"
#include "image/PixelRegion.h"
#include "image/RasterImage.h"

#include <cstddef>

namespace sipi {

/** Decode a crop from the first TIFF image at full stored resolution.
 * Supports tiles or strips, unsigned 8-bit gray, contiguous RGB or RGBA, top-left.
 * Gray requires one MINISBLACK/MINISWHITE channel, normalized to 0 black/255 white.
 * RGB has no extras; RGBA requires exactly one explicitly declared associated or
 * unassociated alpha sample. Unknown/missing extra-sample semantics are rejected.
 * Associated samples normalize to straight alpha with half-up 8-bit rounding;
 * color > alpha is invalid and zero alpha yields zero RGB. Existing precision
 * loss from stored premultiplication cannot be recovered. No scaling, color conversion,
 * pyramid selection, or HTTP I/O. Unsupported layouts throw TiffUnsupportedError
 * (a TiffReadError subtype); corrupt pixel data throws TiffReadError.
 *
 * The crop must already fit the image; this function validates but never clips.
 * It decodes only blocks intersecting the crop, using one reusable tile/strip buffer.
 * Strips span the full width; a single-strip image requires full image decoding.
 * max_decoded_bytes bounds output plus that buffer (default 256 MiB); metadata
 * and libtiff's own allocations retain the separate metadata reader limits.
 * Short/corrupt block reads fail without exposing partial results. Allocation
 * failures may propagate. Concurrent calls own independent TIFF handles and cursors.
 *
 * Borrows an already opened ByteSource; no path is opened or reopened. Tags are
 * checked through a fresh TIFF handle/cursor against that same source version.
 * Reuse the source used for metadata planning. ByteSourceError propagates for I/O
 * failures. Local atomic replacement is safe; in-place file writes are excluded.
 */
[[nodiscard]] RasterImage8 read_tiff_region(const ByteSource& source,
                                        const PixelRegion& region,
                                        std::size_t max_decoded_bytes = 256 * 1024 * 1024);

/** Metadata-level capability check for selecting readable pyramid candidates.
 * Checks sample layout, orientation and configured compression codec. Does not
 * validate encoded data or depth tags; the actual read still checks these. */
[[nodiscard]] bool supports_tiff_region(const TiffDirectory& directory);

/** Decode from an explicitly selected main IFD or SubIFD of the SAME source
 * version that produced directory. The integer region is local to that level.
 * Same supported subset, block selection and budget as the first-image overload.
 * Validates directory dimensions/orientation and rechecks actual decode tags.
 * ICC comes from the main image; conflicting level profiles are rejected.
 * Use only compatible levels returned by read_tiff_metadata. Fractional cropping
 * and scaling of the returned enclosure belong to the subsequent resampler. */
[[nodiscard]] RasterImage8 read_tiff_region(const ByteSource& source,
    const TiffDirectory& directory, const PixelRegion& region,
    std::size_t max_decoded_bytes = 256 * 1024 * 1024);

} // namespace sipi
