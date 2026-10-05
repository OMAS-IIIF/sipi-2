#pragma once
#include "storage/ByteSource.h"
#include "image/RasterImage.h"
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace sipi {
class JpegReadError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
class JpegUnsupportedError : public JpegReadError {
public:
    using JpegReadError::JpegReadError;
};
enum class JpegColor { gray, rgb };

/** Owning header facts for accepted 8-bit gray or RGB/YCbCr JPEG sources.
 * Dimensions are SOF dimensions; accepted EXIF orientation is absent or 1 only.
 * RGB includes ordinary YCbCr JPEG storage; no pixels/color conversion occur here.
 * ICC bytes are retained unchanged, not interpreted as a color transform. */
struct JpegMetadata {
    std::uint32_t width = 0, height = 0;
    JpegColor color = JpegColor::rgb;
    bool progressive = false;
    std::vector<std::uint8_t> icc_profile;
};

/** Read through the first SOS using libjpeg, without decompressing any pixels.
 * Borrows a version-bound ByteSource; owns codec, cursor and all returned data.
 * Independent concurrent calls are safe. Buffered reads may prefetch 16 KiB of
 * compressed data; max_read_bytes bounds cumulative input (default 16 MiB).
 * Saved APP1/APP2 data is bounded by that input limit; ICC assembly may add another
 * copy (at most 16 MiB). This is not a whole-codec allocation/RSS quota.
 * Reject malformed EXIF/ICC, codec errors/warnings, truncation and budget excess
 * with JpegReadError; unsupported precision/color/orientation with
 * JpegUnsupportedError. ByteSource exceptions propagate unchanged. Header success
 * is not validation of the entropy stream, EOI or markers after the first SOS.
 * No pathname reopen, full-object staging, pixel output or shared codec state. */
[[nodiscard]] JpegMetadata read_jpeg_metadata(const ByteSource& source,
    std::size_t max_read_bytes = 16 * 1024 * 1024);
/** Fully decode the same version used for metadata into an owning gray/RGB8 raster.
 * Header facts/ICC are rechecked against metadata; EXIF/color restrictions remain.
 * EXIF/ICC interpretation is limited to markers before the first SOS.
 * Decode every scanline and finish through EOI, including data outside later crops.
 * Fatal warnings, truncation and corruption never return partial pixels. No IDCT
 * downscaling, orientation normalization or ICC conversion. YCbCr is decoded to RGB.
 * Defaults: 256 MiB compressed input and 256 MiB pixel/ICC + multiscan coefficient
 * estimate. Native scratch, saved markers and allocator overhead are additional.
 * Allocation checks precede jpeg_start_decompress; each call owns independent state.
 * ByteSource errors propagate; malformed data/budgets throw JpegReadError.
 */
[[nodiscard]] RasterImage8 read_jpeg_image(const ByteSource& source, const JpegMetadata& metadata,
    std::size_t max_decoded_bytes = 256 * 1024 * 1024,
    std::size_t max_read_bytes = 256 * 1024 * 1024);
}
