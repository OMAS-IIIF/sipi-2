#pragma once

#include <cstdint>
#include "storage/ByteSource.h"
#include <stdexcept>
#include <variant>
#include <vector>

namespace sipi {

/** TIFF orientation values (1..8), retained for later pixel normalization. */
enum class TiffOrientation : std::uint16_t {
    top_left = 1, top_right, bottom_right, bottom_left,
    left_top, right_top, right_bottom, left_bottom
};
struct TiffTiles { std::uint32_t width, height; };
struct TiffStrips { std::uint32_t rows_per_strip; };

/** One TIFF image directory. width/height describe stored pixels, before
 * orientation. Numeric sample/compression/photometric values are TIFF tag codes,
 * not a claim that the pixel decoder supports every represented combination.
 * offset identifies the directory for later TIFFSetSubDirectory calls. */
struct TiffDirectory {
    std::uint64_t offset = 0;
    std::uint32_t width = 0, height = 0;
    TiffOrientation orientation = TiffOrientation::top_left;
    std::uint16_t samples_per_pixel = 0, bits_per_sample = 0;
    std::uint16_t sample_format = 0, photometric = 0, planar_config = 0, compression = 0;
    std::vector<std::uint16_t> extra_samples;
    std::variant<TiffStrips, TiffTiles> layout;

    /** Single grayscale sample (MINISBLACK or MINISWHITE), without extra channels.
     * Describes the color model, not bit-depth/layout decoder support. */
    [[nodiscard]] bool is_grayscale() const noexcept;

    /** Dimensions exposed to IIIF after normalizing TIFF orientation. */
    [[nodiscard]] std::uint32_t display_width() const noexcept;
    [[nodiscard]] std::uint32_t display_height() const noexcept;
};

/** Metadata for the first main image. levels contains only compatible, explicitly
 * marked reduced images, sorted largest first, excluding the full image.
 * ICC bytes are copied unchanged; profile interpretation belongs to color handling.
 * No pointers into libtiff or the borrowed source survive the read. */
struct TiffMetadata {
    TiffDirectory image;
    std::vector<TiffDirectory> levels;
    std::vector<std::uint8_t> icc_profile;
};

/** File/header/metadata/pixel-read failure. Allocation failures may independently propagate. */
class TiffReadError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/** Valid TIFF layout outside the current pixel reader's supported subset. */
class TiffUnsupportedError : public TiffReadError {
public:
    using TiffReadError::TiffReadError;
};

/** Read classic TIFF/BigTIFF directories and delivery metadata without decoding
 * strips, tiles, or scanlines. Supports consecutive reduced main IFDs and reduced
 * SubIFDs (including nested ones); stops the main chain at the next full page.
 * Ambiguous/unmarked thumbnails are not automatically treated as pyramid levels.
 * Borrows an already opened ByteSource for this call. Independent TIFF handles
 * have separate cursors; metadata and pixels can reuse the same source version.
 * ByteSourceError propagates unchanged for storage failures.
 * Per-handle diagnostics and RAII permit independent concurrent reads without
 * changing libtiff's global handlers. Errors throw TiffReadError.
 * Metadata work is bounded to 256 directories, 64 MiB libtiff allocations, and
 * 16 MiB ICC data. Nonfatal libtiff warnings are suppressed in this first reader.
 */
[[nodiscard]] TiffMetadata read_tiff_metadata(const ByteSource& source);

} // namespace sipi
