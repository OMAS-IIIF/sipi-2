#include "JpegReader.h"
#include <cstddef>
#include <cstdio>
#include <jpeglib.h>
#include <algorithm>
#include <array>
#include <csetjmp>
#include <cstring>
#include <exception>
#include <memory>
#include <optional>
#include <span>

namespace sipi {
namespace {
struct ErrorState { jpeg_error_mgr manager{}; std::jmp_buf jump; };
/** Keep mutated state on the heap across longjmp, as in JpegWriter. Callbacks
 * finish C++ exception unwinding before jumping; codec calls own no C++ locals. */
struct Decoder {
    jpeg_decompress_struct codec{};
    ErrorState error;
    jpeg_source_mgr input{};
    std::array<JOCTET, 16384> buffer{};
    const ByteSource& source;
    std::uint64_t offset = 0;
    std::size_t remaining;
    std::exception_ptr failure;
    RasterImage8 image{};
    std::vector<std::uint8_t> scratch_row; // Margin cropping and tail validation.
    Decoder(const ByteSource& value, std::size_t limit) : source(value), remaining(limit) {}
    ~Decoder() { if (codec.mem) jpeg_destroy_decompress(&codec); }
};
void fail(j_common_ptr codec) noexcept {
    std::longjmp(reinterpret_cast<ErrorState*>(codec->err)->jump, 1);
}
void message(j_common_ptr codec, int level) noexcept { if (level < 0) fail(codec); }
void silence(j_common_ptr) noexcept {}
void initialize(j_decompress_ptr) noexcept {}
void terminate(j_decompress_ptr) noexcept {}
boolean fill(j_decompress_ptr codec) noexcept {
    auto& state = *static_cast<Decoder*>(codec->client_data);
    try {
        const auto count = std::min(state.buffer.size(), state.remaining);
        if (!count) throw JpegReadError("JPEG input limit exceeded");
        const auto read = state.source.read_at(state.offset,
            std::as_writable_bytes(std::span{state.buffer.data(), count}));
        if (!read) throw JpegReadError("Truncated JPEG data");
        state.offset += read;
        state.remaining -= read;
        state.input.next_input_byte = state.buffer.data();
        state.input.bytes_in_buffer = read;
    } catch (...) { state.failure = std::current_exception(); }
    if (state.failure) fail(reinterpret_cast<j_common_ptr>(codec));
    return TRUE;
}
void skip(j_decompress_ptr codec, long count) noexcept {
    if (count <= 0) return;
    while (static_cast<std::size_t>(count) > codec->src->bytes_in_buffer) {
        count -= static_cast<long>(codec->src->bytes_in_buffer);
        fill(codec);
    }
    codec->src->next_input_byte += count;
    codec->src->bytes_in_buffer -= static_cast<std::size_t>(count);
}

/** Inspect only EXIF's IFD0 orientation. Bounds-check all accessed bytes; never
 * follow thumbnail/Exif/GPS offsets or substitute EXIF dimensions for JPEG SOF. */
std::optional<unsigned> orientation(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 6 || std::memcmp(bytes.data(), "Exif\0\0", 6)) return {};
    bytes = bytes.subspan(6);
    if (bytes.size() < 8) throw JpegReadError("Truncated EXIF TIFF header");
    const bool little = bytes[0] == 'I' && bytes[1] == 'I';
    if (!little && !(bytes[0] == 'M' && bytes[1] == 'M')) throw JpegReadError("Invalid EXIF byte order");
    const auto word = [&](std::size_t at, unsigned length) -> std::uint32_t {
        if (at > bytes.size() || length > bytes.size() - at) throw JpegReadError("EXIF offset outside marker");
        std::uint32_t value = 0;
        for (unsigned i = 0; i < length; ++i)
            value = (value << 8) | bytes[at + (little ? length - 1 - i : i)];
        return value;
    };
    if (word(2, 2) != 42) throw JpegReadError("Invalid EXIF TIFF signature");
    const auto offset = word(4, 4);
    if (offset < 8) throw JpegReadError("Invalid EXIF IFD0 offset");
    const auto count = word(offset, 2);
    const std::size_t first = static_cast<std::size_t>(offset) + 2;
    if (first > bytes.size() || 4 + count * 12 > bytes.size() - first)
        throw JpegReadError("Truncated EXIF IFD0");
    std::optional<unsigned> result;
    for (unsigned i = 0; i < count; ++i) {
        const auto at = first + i * 12;
        if (word(at, 2) != 0x112) continue;
        if (result || word(at + 2, 2) != 3 || word(at + 4, 4) != 1)
            throw JpegReadError("Invalid or duplicate EXIF orientation");
        result = word(at + 8, 2);
        if (*result < 1 || *result > 8) throw JpegReadError("Invalid EXIF orientation value");
    }
    return result;
}
/** A separate live error boundary for header codec calls. No nontrivial automatic
 * object is created between setjmp and a codec call that can longjmp. */
void read_header(Decoder& state) {
    state.codec.err = jpeg_std_error(&state.error.manager);
    state.error.manager.error_exit = fail;
    state.error.manager.emit_message = message;
    state.error.manager.output_message = silence;
    if (setjmp(state.error.jump)) {
        if (state.failure) std::rethrow_exception(state.failure);
        throw JpegReadError("Invalid JPEG header");
    }
    jpeg_create_decompress(&state.codec);
    state.codec.client_data = &state;
    state.input.init_source = initialize;
    state.input.fill_input_buffer = fill;
    state.input.skip_input_data = skip;
    state.input.resync_to_restart = jpeg_resync_to_restart;
    state.input.term_source = terminate;
    state.codec.src = &state.input;
    jpeg_save_markers(&state.codec, JPEG_APP0 + 1, 0xffff);
    jpeg_save_markers(&state.codec, JPEG_APP0 + 2, 0xffff);
    if (jpeg_read_header(&state.codec, TRUE) != JPEG_HEADER_OK)
        throw JpegReadError("Missing JPEG image header");
}

std::uint64_t coefficient_bytes(const jpeg_decompress_struct& codec);

JpegMetadata extract_metadata(const jpeg_decompress_struct& codec) {
    // All remaining processing is ordinary C++; no further codec calls can jump.
    const bool gray = codec.num_components == 1 && codec.jpeg_color_space == JCS_GRAYSCALE;
    const bool rgb = codec.num_components == 3 &&
        (codec.jpeg_color_space == JCS_RGB || codec.jpeg_color_space == JCS_YCbCr);
    if (codec.data_precision != 8 || (!gray && !rgb))
        throw JpegUnsupportedError("Only 8-bit gray and RGB/YCbCr JPEG headers are supported");
    if (!codec.image_width || !codec.image_height) throw JpegReadError("Invalid JPEG dimensions");
    JpegMetadata result{codec.image_width, codec.image_height,
        gray ? JpegColor::gray : JpegColor::rgb, codec.progressive_mode != 0, {}};
    std::optional<unsigned> exif_orientation;
    std::array<std::span<const std::uint8_t>, 256> chunks{};
    unsigned chunk_count = 0;
    std::size_t total = 0;
    for (auto* marker = codec.marker_list; marker; marker = marker->next) {
        const std::span<const std::uint8_t> bytes{marker->data, marker->data_length};
        if (marker->marker == JPEG_APP0 + 1) {
            if (const auto value = orientation(bytes)) {
                if (exif_orientation) throw JpegReadError("Multiple EXIF orientations");
                exif_orientation = value;
            }
        } else if (bytes.size() >= 12 && !std::memcmp(bytes.data(), "ICC_PROFILE\0", 12)) {
            if (bytes.size() <= 14) throw JpegReadError("Truncated ICC marker");
            const unsigned sequence = bytes[12], count = bytes[13];
            if (!sequence || !count || sequence > count || (chunk_count && count != chunk_count) ||
                !chunks[sequence].empty()) throw JpegReadError("Invalid ICC marker sequence");
            chunk_count = count;
            chunks[sequence] = bytes.subspan(14);
            total += bytes.size() - 14;
            if (total > 16 * 1024 * 1024) throw JpegReadError("JPEG ICC profile exceeds limit");
        }
    }
    if (exif_orientation && *exif_orientation != 1)
        throw JpegUnsupportedError("JPEG EXIF orientation must be normalized on ingestion");
    result.coefficient_bytes = coefficient_bytes(codec);
    result.icc_profile.reserve(total);
    for (unsigned i = 1; i <= chunk_count; ++i) {
        if (chunks[i].empty()) throw JpegReadError("Incomplete JPEG ICC profile");
        result.icc_profile.insert(result.icc_profile.end(), chunks[i].begin(), chunks[i].end());
    }
    return result;
}

/** Bound coefficient arrays before multiscan startup can allocate them. JPEG
 * pads component block grids to sampling-factor multiples. This is an estimate
 * of retained coefficient storage, not a promise about all native scratch. */
std::uint64_t coefficient_bytes(const jpeg_decompress_struct& codec) {
    if (!codec.progressive_mode && codec.comps_in_scan == codec.num_components) return 0;
    std::uint64_t total = 0;
    for (int i = 0; i < codec.num_components; ++i) {
        const auto& component = codec.comp_info[i];
        const auto h = static_cast<unsigned>(component.h_samp_factor);
        const auto v = static_cast<unsigned>(component.v_samp_factor);
        const auto width = ((std::uint64_t(component.width_in_blocks) + h - 1) / h) * h;
        const auto height = ((std::uint64_t(component.height_in_blocks) + v - 1) / v) * v;
        total += width * height * DCTSIZE2 * sizeof(JCOEF);
    }
    return total;
}

/** Decode phase 1 (own error boundary): start decompression and, for a partial
 * region, narrow output to the iMCU-aligned columns covering [x, x + width).
 * libjpeg-turbo moves x left to an iMCU boundary and widens width to match. */
void start_region(Decoder& state, PixelLayout layout, std::uint32_t full_width,
                  std::uint32_t full_height, JDIMENSION& x, JDIMENSION& width) {
    if (setjmp(state.error.jump)) {
        if (state.failure) std::rethrow_exception(state.failure);
        throw JpegReadError("Invalid JPEG pixel data");
    }
    state.codec.out_color_space = layout == PixelLayout::gray ? JCS_GRAYSCALE : JCS_RGB;
    // Explicit defaults form part of the rendering recipe.
    state.codec.dct_method = JDCT_ISLOW;
    state.codec.do_fancy_upsampling = TRUE;
    if (!jpeg_start_decompress(&state.codec) || state.codec.output_width != full_width ||
        state.codec.output_height != full_height ||
        state.codec.output_components != (layout == PixelLayout::gray ? 1 : 3))
        throw JpegReadError("Unexpected JPEG output layout");
    if (x != 0 || width != full_width) {
        jpeg_crop_scanline(&state.codec, &x, &width);
        if (state.codec.output_width != width) throw JpegReadError("Unexpected JPEG crop width");
    }
}

/** Decode phase 2 (own error boundary): skip rows above the region, decode its
 * rows into state.image, skip the rest and finish through EOI. Skipped rows are
 * still entropy-decoded, so the whole stream is validated. */
void decode_rows(Decoder& state, std::uint32_t first_row, std::uint32_t left_margin) {
    if (setjmp(state.error.jump)) {
        if (state.failure) std::rethrow_exception(state.failure);
        throw JpegReadError("Invalid JPEG pixel data");
    }
    if (first_row && jpeg_skip_scanlines(&state.codec, first_row) != first_row)
        throw JpegReadError("Incomplete JPEG scanline");
    const std::size_t row_bytes = std::size_t(state.image.width) * state.image.channel_count();
    while (state.codec.output_scanline < first_row + state.image.height) {
        auto* target = state.image.pixels.data() + std::size_t(state.codec.output_scanline - first_row) * row_bytes;
        const bool crop = state.codec.output_width != state.image.width;
        JSAMPROW row = crop ? state.scratch_row.data() : target;
        if (jpeg_read_scanlines(&state.codec, &row, 1) != 1)
            throw JpegReadError("Incomplete JPEG scanline");
        if (crop) std::memcpy(target, row + std::size_t(left_margin) * state.image.channel_count(), row_bytes);
    }
    const auto rest = state.codec.output_height - state.codec.output_scanline;
    // Skipping exactly to the bottom makes libjpeg-turbo mark EOI reached
    // without consuming the tail. Leave the last row for a real read so all
    // entropy data and the EOI marker are still validated. Use an owned row.
    if (rest) {
        if (rest > 1 && jpeg_skip_scanlines(&state.codec, rest - 1) != rest - 1)
            throw JpegReadError("Incomplete JPEG scanline");
        // The requested rows must remain intact; the decoder owns this scratch
        // row across the longjmp boundary (allocated before entering here).
        JSAMPROW row = state.scratch_row.data();
        if (jpeg_read_scanlines(&state.codec, &row, 1) != 1)
            throw JpegReadError("Incomplete JPEG scanline");
    }
    if (!jpeg_finish_decompress(&state.codec)) throw JpegReadError("Incomplete JPEG image");
}
} // namespace

JpegMetadata read_jpeg_metadata(const ByteSource& source, std::size_t max_read_bytes) {
    const auto state = std::make_unique<Decoder>(source, max_read_bytes);
    read_header(*state);
    return extract_metadata(state->codec);
}

JpegRegion read_jpeg_region(const ByteSource& source, const JpegMetadata& metadata,
                            const PixelRegion& region, std::size_t max_decoded_bytes,
                            std::size_t max_read_bytes) {
    if (!region.width || !region.height || region.x >= metadata.width || region.y >= metadata.height ||
        region.width > metadata.width - region.x || region.height > metadata.height - region.y)
        throw JpegReadError("JPEG region outside the image");
    const auto state = std::make_unique<Decoder>(source, std::min(max_read_bytes, std::size_t{16 * 1024 * 1024}));
    read_header(*state);
    auto actual = extract_metadata(state->codec);
    if (actual.width != metadata.width || actual.height != metadata.height ||
        actual.color != metadata.color || actual.progressive != metadata.progressive ||
        actual.icc_profile != metadata.icc_profile || actual.coefficient_bytes != metadata.coefficient_bytes)
        throw JpegReadError("JPEG header changed since metadata planning");
    const std::uint64_t channels = actual.color == JpegColor::gray ? 1 : 3;
    // Exact result raster plus one full-width upper bound for the scratch row.
    // No padded raster survives into later resize/watermark budget checks.
    const bool has_tail = region.y + region.height < actual.height;
    const bool needs_scratch = has_tail || region.width != actual.width;
    const auto pixels = (std::uint64_t(region.width) * region.height +
                         (needs_scratch ? actual.width : 0u)) * channels;
    const auto coefficients = actual.coefficient_bytes;
    if (pixels > max_decoded_bytes || coefficients > max_decoded_bytes - pixels ||
        actual.icc_profile.size() > max_decoded_bytes - pixels - coefficients)
        throw JpegReadError("JPEG decoding exceeds working memory limit");
    const auto layout = channels == 1 ? PixelLayout::gray : PixelLayout::rgb;
    // The header remains capped at 16 MiB; the total decode uses the larger
    // cumulative input budget, including already-prefetched bytes.
    state->remaining = max_read_bytes - static_cast<std::size_t>(state->offset);
    // Fancy upsampling of horizontally subsampled chroma interpolates with the
    // neighboring chroma column, which a crop does not decode. One extra iMCU on
    // each side keeps those edge effects outside the requested columns.
    // One iMCU: the widest horizontal sampling factor times the block size.
    const JDIMENSION margin = static_cast<JDIMENSION>(std::max(1, state->codec.max_h_samp_factor)) * DCTSIZE;
    const JDIMENSION left = region.x > margin ? region.x - margin : 0;
    const JDIMENSION right = std::min<JDIMENSION>(actual.width, region.x + region.width + margin);
    JDIMENSION x = left, width = right - left;
    start_region(*state, layout, actual.width, actual.height, x, width);
    // Allocate outside the codec error boundaries (no C++ objects across longjmp).
    state->image = {region.width, region.height,
        std::vector<std::uint8_t>(static_cast<std::size_t>(region.width) * region.height * channels),
        {}, layout};
    if (needs_scratch) state->scratch_row.resize(static_cast<std::size_t>(width) * channels);
    decode_rows(*state, region.y, region.x - x);
    state->image.icc_profile = std::move(actual.icc_profile);
    return {std::move(state->image), region.x, region.y};
}

RasterImage8 read_jpeg_image(const ByteSource& source, const JpegMetadata& metadata,
                            std::size_t max_decoded_bytes, std::size_t max_read_bytes) {
    return read_jpeg_region(source, metadata, {0, 0, metadata.width, metadata.height},
                            max_decoded_bytes, max_read_bytes).image;
}
} // namespace sipi
