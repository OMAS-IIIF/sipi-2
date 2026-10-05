#pragma once
#include "storage/ByteSource.h"
#include "image/RasterImage.h"
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace sipi {
class PngReadError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
class PngUnsupportedError : public PngReadError {
public:
    using PngReadError::PngReadError;
};
/** Original PNG color model, before palette/alpha expansion. */
enum class PngColor { gray, rgb, palette, gray_alpha, rgba };

/** Owning pre-IDAT facts; dimensions are the IHDR canvas dimensions.
 * Transparency includes either an alpha channel or a tRNS chunk. No pixel,
 * palette expansion, color conversion or orientation normalization occurs here. */
struct PngMetadata {
    std::uint32_t width = 0, height = 0;
    PngColor color = PngColor::rgb;
    unsigned bit_depth = 8;
    bool interlaced = false;
    bool transparency = false;
    std::vector<std::uint8_t> icc_profile;
    std::optional<unsigned> srgb_intent = std::nullopt;
    bool color_declaration = false; // Traditional color chunks; ICC or explicit sRGB is required for delivery.
    bool operator==(const PngMetadata&) const = default;
};

/** Read through the first IDAT header using libpng over a borrowed version-bound
 * source. Each call owns independent codec/buffer state; concurrent calls are safe.
 * Accept legal 1/2/4/8-bit gray/palette and 8-bit RGB/gray-alpha/RGBA, including
 * Adam7. Reject 16-bit, eXIf, APNG and CICP/HDR headers with PngUnsupportedError.
 * Warnings, invalid CRCs, malformed headers and exceeded limits throw PngReadError;
 * ByteSource failures propagate unchanged. ICC is decompressed and retained, with
 * libpng validation but no color transform. Other color/text metadata is not retained.
 * Default input and per-chunk allocation limits: 16 MiB each; dimensions <=1,000,000.
 * Buffered reads may prefetch 16 KiB. Native bookkeeping and ICC copy are additional,
 * not a process-wide memory quota. IDAT pixels/CRC, IEND and post-IDAT metadata are
 * not validated; success does not imply that the complete image can be decoded.
 */
[[nodiscard]] PngMetadata read_png_metadata(const ByteSource& source,
    std::size_t max_read_bytes = 16 * 1024 * 1024);
/** Whether the current raster/encoder pipeline can preserve this color encoding.
 * Explicit sRGB is supported; gamma/chromaticity alone still require color management. */
[[nodiscard]] bool supports_png_pixels(const PngMetadata& metadata) noexcept;

/** Fully decode and validate through IEND into gray, RGB or straight RGBA8.
 * Expands palette, low-bit gray and tRNS losslessly; gray+alpha stays native gray-alpha.
 * Adam7 is combined in one zero-initialized owning raster. Rechecks header facts.
 * No gamma/ICC transform, EXIF normalization or animation. Reject unsupported
 * color encodings rather than discard their declarations. Errors/warnings/CRC and
 * truncation never return partial pixels; ByteSource exceptions propagate.
 * Defaults: 256 MiB cumulative input; 256 MiB raster+ICC. Native rows, inflate,
 * markers and allocator overhead are additional. Header input stays <=16 MiB.
 * Each call owns its state, with no pathname reopen or full compressed-file copy.
 */
[[nodiscard]] RasterImage8 read_png_image(const ByteSource& source, const PngMetadata& metadata,
    std::size_t max_decoded_bytes = 256 * 1024 * 1024,
    std::size_t max_read_bytes = 256 * 1024 * 1024);
} // namespace sipi
