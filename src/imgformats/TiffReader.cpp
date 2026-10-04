#include "TiffReader.h"

#include <tiffio.h>

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <unordered_set>

namespace sipi {
namespace {
constexpr std::size_t directory_limit = 256;
constexpr std::uint32_t icc_limit = 16 * 1024 * 1024;

/** C callbacks must not throw; keep error text in bounded, per-call storage. */
struct Diagnostics { std::array<char, 512> error{}; };
int capture_error(TIFF*, void* context, const char*, const char* format, va_list args) noexcept {
    auto& diagnostic = *static_cast<Diagnostics*>(context);
    std::vsnprintf(diagnostic.error.data(), diagnostic.error.size(), format, args);
    return 1;
}
int ignore_warning(TIFF*, void*, const char*, const char*, va_list) noexcept { return 1; }

/** Each libtiff handle borrows one source and owns its own logical cursor.
 * C callbacks capture exceptions; the enclosing C++ reader rethrows them only
 * after libtiff returns. A failed source is never read again by this handle.
 */
struct TiffInput {
    const ByteSource& source;
    std::uint64_t position = 0;
    std::exception_ptr failure;
    Diagnostics diagnostic;

    void check_source() const { if (failure) std::rethrow_exception(failure); }
};

tmsize_t read_bytes(thandle_t handle, void* buffer, tmsize_t count) noexcept {
    auto& input = *static_cast<TiffInput*>(handle);
    if (input.failure) return -1;
    try {
        if (count < 0 || (count > 0 && !buffer))
            throw ByteSourceError(ByteSourceErrorCode::invalid_argument, "Invalid TIFF source read");
        const auto size = input.source.info().size;
        const auto wanted = input.position >= size ? 0 :
            std::min<std::uint64_t>(static_cast<std::uint64_t>(count), size - input.position);
        const auto read = input.source.read_at(input.position,
            {static_cast<std::byte*>(buffer), static_cast<std::size_t>(count)});
        if (read != wanted)
            throw ByteSourceError(ByteSourceErrorCode::io, "Byte source returned an unexpected read length");
        input.position += read; // bounded by source size, already checked
        return static_cast<tmsize_t>(read);
    } catch (...) {
        input.failure = std::current_exception();
        return -1;
    }
}
tmsize_t reject_write(thandle_t, void*, tmsize_t) noexcept { return -1; }

/** libtiff passes negative relative offsets encoded in unsigned toff_t.
 * Handle both directions explicitly without signed casts or arithmetic overflow.
 * Seeking past EOF is allowed; reading there returns zero. -1 is reserved for
 * failure and cannot be a valid cursor position.
 */
toff_t seek_bytes(thandle_t handle, toff_t offset, int origin) noexcept {
    auto& input = *static_cast<TiffInput*>(handle);
    constexpr auto invalid = std::numeric_limits<toff_t>::max();
    if (input.failure) return invalid;
    std::uint64_t base;
    switch (origin) {
    case SEEK_SET:
        if (offset == invalid) return invalid;
        input.position = offset;
        return offset;
    case SEEK_CUR: base = input.position; break;
    case SEEK_END: base = input.source.info().size; break;
    default: return invalid;
    }
    if (offset > invalid / 2) {
        const auto distance = invalid - offset + 1;
        if (distance > base) return invalid;
        input.position = base - distance;
    } else {
        if (offset >= invalid - base) return invalid;
        input.position = base + offset;
    }
    return input.position;
}
int close_input(thandle_t) noexcept { return 0; } // source belongs to caller
toff_t source_size(thandle_t handle) noexcept {
    return static_cast<TiffInput*>(handle)->source.info().size;
}
int no_mapping(thandle_t, void**, toff_t*) noexcept { return 0; }
void unmap_input(thandle_t, void*, toff_t) noexcept {}

/** Context must outlive the TIFF handle; closing it never closes the source. */
using TiffHandle = std::unique_ptr<TIFF, decltype(&TIFFClose)>;
TiffHandle open_tiff(TiffInput& input) {
    if (input.source.info().size == std::numeric_limits<toff_t>::max())
        throw TiffReadError("TIFF source size exceeds offset range");
    std::unique_ptr<TIFFOpenOptions, decltype(&TIFFOpenOptionsFree)> options(TIFFOpenOptionsAlloc(), TIFFOpenOptionsFree);
    if (!options) { throw std::bad_alloc{}; }
    TIFFOpenOptionsSetErrorHandlerExtR(options.get(), capture_error, &input.diagnostic);
    TIFFOpenOptionsSetWarningHandlerExtR(options.get(), ignore_warning, nullptr);
    TIFFOpenOptionsSetMaxSingleMemAlloc(options.get(), 64 * 1024 * 1024);
    TIFFOpenOptionsSetMaxCumulatedMemAlloc(options.get(), 64 * 1024 * 1024);
    // No mmap and no strip chopping. Diagnostic name reveals no storage identity.
    TiffHandle file(TIFFClientOpenExt("image source", "rmc", &input, read_bytes, reject_write,
        seek_bytes, close_input, source_size, no_mapping, unmap_input, options.get()), TIFFClose);
    input.check_source();
    if (!file) { throw TiffReadError("Cannot open TIFF: " + std::string(input.diagnostic.error.data())); }
    return file;
}

/** Copy only the fields needed to describe pixel layout and plan delivery. */
TiffDirectory read_directory(TIFF* file) {
    TiffDirectory result;
    result.offset = TIFFCurrentDirOffset(file);
    if (!TIFFGetField(file, TIFFTAG_IMAGEWIDTH, &result.width) ||
        !TIFFGetField(file, TIFFTAG_IMAGELENGTH, &result.height) ||
        result.width == 0 || result.height == 0) {
        throw TiffReadError("TIFF requires positive width and height");
    }
    std::uint16_t orientation = ORIENTATION_TOPLEFT;
    TIFFGetFieldDefaulted(file, TIFFTAG_ORIENTATION, &orientation);
    if (orientation < 1 || orientation > 8) { throw TiffReadError("Invalid TIFF orientation"); }
    result.orientation = static_cast<TiffOrientation>(orientation);
    TIFFGetFieldDefaulted(file, TIFFTAG_SAMPLESPERPIXEL, &result.samples_per_pixel);
    TIFFGetFieldDefaulted(file, TIFFTAG_BITSPERSAMPLE, &result.bits_per_sample);
    TIFFGetFieldDefaulted(file, TIFFTAG_SAMPLEFORMAT, &result.sample_format);
    TIFFGetFieldDefaulted(file, TIFFTAG_PLANARCONFIG, &result.planar_config);
    TIFFGetFieldDefaulted(file, TIFFTAG_COMPRESSION, &result.compression);
    if (!TIFFGetField(file, TIFFTAG_PHOTOMETRIC, &result.photometric) ||
        result.samples_per_pixel == 0 || result.bits_per_sample == 0) {
        throw TiffReadError("TIFF requires photometric interpretation and valid sample dimensions");
    }
    std::uint16_t count = 0;
    std::uint16_t* extra = nullptr;
    if (TIFFGetField(file, TIFFTAG_EXTRASAMPLES, &count, &extra) && count > 0) {
        if (!extra || count > result.samples_per_pixel) { throw TiffReadError("Invalid TIFF extra samples"); }
        result.extra_samples.assign(extra, extra + count);
    }
    if (TIFFIsTiled(file)) {
        TiffTiles tiles{};
        if (!TIFFGetField(file, TIFFTAG_TILEWIDTH, &tiles.width) ||
            !TIFFGetField(file, TIFFTAG_TILELENGTH, &tiles.height) ||
            tiles.width == 0 || tiles.height == 0) {
            throw TiffReadError("Invalid TIFF tile dimensions");
        }
        result.layout = tiles;
    } else {
        std::uint32_t rows = 0;
        TIFFGetFieldDefaulted(file, TIFFTAG_ROWSPERSTRIP, &rows);
        if (rows == 0) { throw TiffReadError("Invalid TIFF strip dimensions"); }
        result.layout = TiffStrips{std::min(rows, result.height)};
    }
    return result;
}

/** Own profile bytes before changing the current directory. */
std::vector<std::uint8_t> read_profile(TIFF* file) {
    std::uint32_t size = 0;
    void* data = nullptr;
    if (!TIFFGetField(file, TIFFTAG_ICCPROFILE, &size, &data) || size == 0) { return {}; }
    if (!data || size > icc_limit) { throw TiffReadError("TIFF ICC profile exceeds metadata limit"); }
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    return {bytes, bytes + size};
}

bool marked_reduced(TIFF* file) {
    std::uint32_t type = 0;
    TIFFGetFieldDefaulted(file, TIFFTAG_SUBFILETYPE, &type);
    return (type & FILETYPE_REDUCEDIMAGE) != 0 && (type & FILETYPE_MASK) == 0;
}

/** A reduced flag alone is insufficient: layout interpretation and aspect ratio
 * must agree. Tolerate at most one source pixel's cross-product rounding error. */
bool compatible_level(const TiffDirectory& base, const TiffDirectory& level) {
    if (level.width > base.width || level.height > base.height ||
        (level.width == base.width && level.height == base.height) ||
        level.orientation != base.orientation || level.samples_per_pixel != base.samples_per_pixel ||
        level.bits_per_sample != base.bits_per_sample || level.sample_format != base.sample_format ||
        level.photometric != base.photometric || level.planar_config != base.planar_config ||
        level.extra_samples != base.extra_samples) {
        return false;
    }
    const auto a = static_cast<std::uint64_t>(base.width) * level.height;
    const auto b = static_cast<std::uint64_t>(base.height) * level.width;
    return (a > b ? a - b : b - a) <= std::max(base.width, base.height);
}

/** Copy offsets before changing directories: libtiff owns the source array. */
void append_subifds(TIFF* file, std::vector<std::uint64_t>& pending) {
    std::uint16_t count = 0;
    std::uint64_t* offsets = nullptr;
    if (TIFFGetField(file, TIFFTAG_SUBIFD, &count, &offsets) && count > 0) {
        if (!offsets || count > directory_limit || pending.size() > directory_limit - count) {
            throw TiffReadError("Too many TIFF subdirectories");
        }
        pending.insert(pending.end(), offsets, offsets + count);
    }
}
} // namespace

std::uint32_t TiffDirectory::display_width() const noexcept {
    return static_cast<std::uint16_t>(orientation) >= 5 ? height : width;
}
std::uint32_t TiffDirectory::display_height() const noexcept {
    return static_cast<std::uint16_t>(orientation) >= 5 ? width : height;
}

TiffMetadata read_tiff_metadata(const ByteSource& source) {
    TiffInput input{source, 0, {}, {}};
    auto file = open_tiff(input);
    TiffMetadata result;
    result.image = read_directory(file.get());
    result.icc_profile = read_profile(file.get());
    std::vector<std::uint64_t> pending;
    append_subifds(file.get(), pending);
    std::unordered_set<std::uint64_t> visited{result.image.offset};
    while (!TIFFLastDirectory(file.get())) {
        if (!TIFFReadDirectory(file.get())) {
            input.check_source();
            throw TiffReadError("Cannot read TIFF main directory");
        }
        if (!visited.insert(TIFFCurrentDirOffset(file.get())).second || visited.size() > directory_limit) {
            throw TiffReadError("Cyclic or excessive TIFF directory chain");
        }
        if (!marked_reduced(file.get())) { break; } // Next full page is not our image.
        auto level = read_directory(file.get());
        const auto profile = read_profile(file.get());
        if (compatible_level(result.image, level) &&
            (profile.empty() || profile == result.icc_profile)) {
            append_subifds(file.get(), pending);
            result.levels.push_back(std::move(level));
        }
    }
    for (std::size_t i = 0; i < pending.size(); ++i) {
        const auto offset = pending[i];
        if (offset == 0 || !visited.insert(offset).second) {
            throw TiffReadError("Invalid or repeated TIFF SubIFD offset");
        }
        if (visited.size() > directory_limit || !TIFFSetSubDirectory(file.get(), offset)) {
            input.check_source();
            throw TiffReadError("Cannot read TIFF SubIFD or directory limit exceeded");
        }
        if (marked_reduced(file.get())) {
            auto level = read_directory(file.get());
            const auto profile = read_profile(file.get());
            if (compatible_level(result.image, level) &&
                (profile.empty() || profile == result.icc_profile)) {
                append_subifds(file.get(), pending);
                result.levels.push_back(std::move(level));
            }
        }
    }
    input.check_source();
    if (input.diagnostic.error[0] != '\0') { throw TiffReadError(input.diagnostic.error.data()); }
    std::sort(result.levels.begin(), result.levels.end(), [](const auto& a, const auto& b) {
        return static_cast<std::uint64_t>(a.width) * a.height > static_cast<std::uint64_t>(b.width) * b.height;
    });
    return result;
}
} // namespace sipi

namespace sipi {
namespace {
/** Check products before multiplying, with the caller's finite allocation bound. */
std::size_t raster_bytes(std::uint32_t width, std::uint32_t height, std::size_t channels, std::size_t limit) {
    if (width == 0 || height == 0 || width > limit / channels || height > limit / (static_cast<std::size_t>(width) * channels)) {
        throw TiffReadError("Raster exceeds the decoded-byte limit");
    }
    return static_cast<std::size_t>(width) * height * channels;
}
} // namespace

bool TiffDirectory::is_grayscale() const noexcept {
    return samples_per_pixel == 1 && extra_samples.empty() &&
        (photometric == PHOTOMETRIC_MINISBLACK || photometric == PHOTOMETRIC_MINISWHITE);
}

bool supports_tiff_region(const TiffDirectory& directory) {
    const bool rgb = directory.samples_per_pixel == 3 && directory.extra_samples.empty();
    const bool rgba = directory.samples_per_pixel == 4 && directory.extra_samples.size() == 1 &&
        (directory.extra_samples[0] == EXTRASAMPLE_ASSOCALPHA ||
         directory.extra_samples[0] == EXTRASAMPLE_UNASSALPHA);
    return (((rgb || rgba) && directory.photometric == PHOTOMETRIC_RGB) || directory.is_grayscale()) &&
        directory.orientation == TiffOrientation::top_left &&
        directory.bits_per_sample == 8 &&
        directory.sample_format == SAMPLEFORMAT_UINT &&
        directory.planar_config == PLANARCONFIG_CONTIG &&
        TIFFIsCODECConfigured(directory.compression);
}

namespace {
RasterImage8 decode_region(const ByteSource& source, const PixelRegion& region,
                        const TiffDirectory* selected, std::size_t max_decoded_bytes) {
    TiffInput input{source, 0, {}, {}};
    auto file = open_tiff(input);
    // The main image owns the color interpretation; compatible reduced IFDs
    // may omit ICC. Keep it even after switching to a main-chain IFD or SubIFD.
    auto profile = read_profile(file.get());
    if (selected && (!selected->offset || !TIFFSetSubDirectory(file.get(), selected->offset))) {
        input.check_source();
        throw TiffReadError("Cannot select TIFF image directory");
    }
    const auto metadata = read_directory(file.get());
    if (selected && (metadata.width != selected->width || metadata.height != selected->height ||
        metadata.orientation != selected->orientation))
        throw TiffReadError("TIFF directory differs from the read plan");
    if (selected) {
        const auto level_profile = read_profile(file.get());
        if (!level_profile.empty() && level_profile != profile)
            throw TiffUnsupportedError("TIFF level has a different color profile");
    }
    const auto* tiles = std::get_if<TiffTiles>(&metadata.layout);
    std::uint32_t depth = 1, tile_depth = 1;
    TIFFGetFieldDefaulted(file.get(), TIFFTAG_IMAGEDEPTH, &depth);
    TIFFGetFieldDefaulted(file.get(), TIFFTAG_TILEDEPTH, &tile_depth);
    if (!supports_tiff_region(metadata) || depth != 1 || tile_depth != 1) {
        throw TiffUnsupportedError("Unsupported TIFF: expected top-left unsigned 8-bit gray, contiguous RGB or explicitly tagged RGBA");
    }
    if (region.width == 0 || region.height == 0 || region.x >= metadata.width || region.y >= metadata.height ||
        region.width > metadata.width - region.x || region.height > metadata.height - region.y) {
        throw TiffReadError("TIFF crop must be nonempty and entirely inside the image");
    }
    const auto channels = metadata.samples_per_pixel;
    const auto output_bytes = raster_bytes(region.width, region.height, channels, max_decoded_bytes);
    // A strip spans the full image width; tiles and strips otherwise share the
    // same rectangle-intersection/copy logic. The final strip may be shorter.
    const auto block_width = tiles ? tiles->width : metadata.width;
    const auto block_height = tiles ? tiles->height : std::get<TiffStrips>(metadata.layout).rows_per_strip;
    const std::uint64_t right = static_cast<std::uint64_t>(region.x) + region.width;
    const std::uint64_t bottom = static_cast<std::uint64_t>(region.y) + region.height;
    const std::uint64_t first_x = region.x / block_width * static_cast<std::uint64_t>(block_width);
    const std::uint64_t first_y = region.y / block_height * static_cast<std::uint64_t>(block_height);
    const auto buffer_rows = tiles ? block_height :
        static_cast<std::uint32_t>(std::min<std::uint64_t>(block_height, metadata.height - first_y));
    const auto buffer_bytes = raster_bytes(block_width, buffer_rows, channels, max_decoded_bytes - output_bytes);
    if (buffer_bytes > static_cast<std::size_t>(std::numeric_limits<tmsize_t>::max()) ||
        (tiles && TIFFTileSize64(file.get()) != buffer_bytes) ||
        (!tiles && TIFFScanlineSize64(file.get()) != static_cast<std::uint64_t>(block_width) * channels))
        throw TiffReadError("Unexpected TIFF decoded block size");
    input.check_source();
    if (input.diagnostic.error[0] != '\0') { throw TiffReadError(input.diagnostic.error.data()); }
    RasterImage8 output{region.width, region.height, std::vector<std::uint8_t>(output_bytes), std::move(profile),
        channels == 1 ? PixelLayout::gray : (channels == 4 ? PixelLayout::rgba : PixelLayout::rgb)};
    std::vector<std::uint8_t> block(buffer_bytes);
    // uint64 positions avoid overflow at UINT32_MAX edges. Codec coordinates
    // are inside the image and still fit uint32_t.
    for (auto by = first_y; by < bottom; by += block_height) {
        const auto rows = tiles ? block_height :
            static_cast<std::uint32_t>(std::min<std::uint64_t>(block_height, metadata.height - by));
        const auto expected_bytes = raster_bytes(block_width, rows, channels, buffer_bytes);
        for (auto bx = first_x; bx < right; bx += block_width) {
            tmsize_t count;
            if (tiles) {
                const auto index = TIFFComputeTile(file.get(), static_cast<std::uint32_t>(bx),
                                                    static_cast<std::uint32_t>(by), 0, 0);
                count = TIFFReadEncodedTile(file.get(), index, block.data(), static_cast<tmsize_t>(expected_bytes));
            } else {
                const auto index = TIFFComputeStrip(file.get(), static_cast<std::uint32_t>(by), 0);
                count = TIFFReadEncodedStrip(file.get(), index, block.data(), static_cast<tmsize_t>(expected_bytes));
            }
            if (count < 0 || static_cast<std::size_t>(count) != expected_bytes || input.diagnostic.error[0] != '\0') {
                input.check_source();
                throw TiffReadError(std::string(tiles ? "Cannot decode complete TIFF tile: " :
                    "Cannot decode complete TIFF strip: ") + input.diagnostic.error.data());
            }
            const auto left = std::max<std::uint64_t>(bx, region.x);
            const auto top = std::max<std::uint64_t>(by, region.y);
            const auto end_x = std::min(bx + block_width, right);
            const auto end_y = std::min(by + rows, bottom);
            const auto row_bytes = static_cast<std::size_t>(end_x - left) * channels;
            for (auto y = top; y < end_y; ++y) {
                const auto source = static_cast<std::size_t>((y - by) * block_width + left - bx) * channels;
                const auto destination = static_cast<std::size_t>((y - region.y) * region.width + left - region.x) * channels;
                std::memcpy(output.pixels.data() + destination, block.data() + source, row_bytes);
            }
        }
    }
    if (metadata.is_grayscale() && metadata.photometric == PHOTOMETRIC_MINISWHITE)
        for (auto& value : output.pixels) value = static_cast<std::uint8_t>(255 - value);
    input.check_source();
    if (channels == 4 && metadata.extra_samples[0] == EXTRASAMPLE_ASSOCALPHA) {
        // Normalize only requested pixels, not unused tile padding. Division
        // cannot recover precision lost when 8-bit premultiplication was stored.
        for (std::size_t i = 0; i < output.pixels.size(); i += 4) {
            const unsigned alpha = output.pixels[i+3];
            for (unsigned c = 0; c < 3; ++c) {
                const unsigned value = output.pixels[i+c];
                if (value > alpha) throw TiffReadError("Invalid associated TIFF alpha sample");
                output.pixels[i+c] = alpha ? static_cast<std::uint8_t>((value*255 + alpha/2)/alpha) : 0;
            }
        }
    }
    return output;
}
} // namespace

RasterImage8 read_tiff_region(const ByteSource& source, const PixelRegion& region,
                           std::size_t max_decoded_bytes) {
    return decode_region(source, region, nullptr, max_decoded_bytes);
}

RasterImage8 read_tiff_region(const ByteSource& source, const TiffDirectory& directory,
                           const PixelRegion& region, std::size_t max_decoded_bytes) {
    return decode_region(source, region, &directory, max_decoded_bytes);
}
} // namespace sipi
