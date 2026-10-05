#pragma once

#include "image/RasterImage.h"
#include <functional>
#include <span>
#include <stdexcept>

namespace sipi {
/** Invalid raster/profile or PNG codec failure. Sink exceptions propagate unchanged. */
class PngWriteError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/** Synchronous consumer of borrowed bytes; consume/copy during the call. */
using PngSink = std::function<void(std::span<const std::uint8_t>)>;

/** Write lossless, noninterlaced gray8, gray-alpha8, RGB8 or straight RGBA8 PNG in blocks of at most 16 KiB.
 * Borrows image and sink for this call; independent calls share no codec state.
 * Emits scanlines without a second raster or full encoded-image buffer. Uses
 * compression level 6. Color and alpha bytes are unchanged: no color/gamma conversion and
 * no sRGB assumption. ICC is preserved as iCCP after basic matching GRAY/RGB header checks
 * and libpng's size checks. Rejected profiles/warnings fail rather than silently
 * dropping metadata. This is NOT full ICC validation or color management.
 * Explicit srgb_intent emits an sRGB chunk; simultaneous ICC/sRGB is rejected.
 * Untagged input remains untagged; no EXIF/IPTC metadata is generated.
 *
 * Positive dimensions are limited to 1,000,000 per axis; exact w*h*channel_count() pixel data
 * is required and limited to 256 MiB, ICC to 16 MiB. These bound input, not total
 * process memory. libpng also allocates row/filter/compression/profile buffers.
 * Output/storage limits belong to the sink. On any failure, discard partial
 * output. C callbacks contain exceptions; RAII releases codec state after errors.
 */
void write_png(const RasterImage8& image, const PngSink& sink);
} // namespace sipi
