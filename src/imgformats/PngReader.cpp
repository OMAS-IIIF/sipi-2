#include "PngReader.h"
#include <png.h>
#include <algorithm>
#include <array>
#include <csetjmp>
#include <cstring>
#include <cstdio>
#include <exception>
#include <memory>

namespace sipi {
namespace {
/** Mutated state stays on the heap across libpng's longjmp error boundary. */
struct Decoder {
    png_structp codec = nullptr;
    png_infop info = nullptr;
    png_infop end = nullptr;
    RasterImage8 image{};
    bool unsupported = false;
    const ByteSource& source;
    std::array<std::byte, 16384> buffer{};
    std::size_t position = 0, available = 0, remaining;
    std::uint64_t offset = 0;
    std::exception_ptr failure;
    std::array<char, 256> message{};
    Decoder(const ByteSource& input, std::size_t limit) : source(input), remaining(limit) {}
    ~Decoder() { if (codec) png_destroy_read_struct(&codec, &info, &end); }
};
void fail(png_structp codec, png_const_charp message) noexcept {
    auto& state = *static_cast<Decoder*>(png_get_error_ptr(codec));
    std::snprintf(state.message.data(), state.message.size(), "PNG: %s", message);
    png_longjmp(codec, 1);
}

/** Complete C++ unwinding before jumping back out of the native codec. */
void read(png_structp codec, png_bytep output, png_size_t count) noexcept {
    auto& state = *static_cast<Decoder*>(png_get_io_ptr(codec));
    try {
        while (count) {
            if (state.position == state.available) {
                const auto request = std::min(state.remaining, state.buffer.size());
                if (!request) throw PngReadError("PNG input limit exceeded");
                state.available = state.source.read_at(state.offset, std::span{state.buffer}.first(request));
                if (!state.available) throw PngReadError("Truncated PNG data");
                state.offset += state.available;
                state.remaining -= state.available;
                state.position = 0;
            }
            const auto length = std::min<std::size_t>(count, state.available - state.position);
            std::memcpy(output, state.buffer.data() + state.position, length);
            state.position += length;
            output += length;
            count -= length;
        }
    } catch (...) { state.failure = std::current_exception(); }
    if (state.failure) png_longjmp(codec, 1);
}

/** APNG is not a still-image source. Reject animation chunks anywhere, including
 * after IDAT, instead of silently serving only its default image. */
int unknown_chunk(png_structp codec, png_unknown_chunkp chunk) noexcept {
    if (!std::memcmp(chunk->name, "acTL", 4) || !std::memcmp(chunk->name, "fcTL", 4) ||
        !std::memcmp(chunk->name, "fdAT", 4)) {
        static_cast<Decoder*>(png_get_user_chunk_ptr(codec))->unsupported = true;
        png_error(codec, "Animated PNG requires ingestion normalization");
    }
    // Discard unrelated ancillary data; never hide an unknown critical chunk.
    return (chunk->name[0] & 32) ? 1 : 0;
}

/** Only trivial automatic objects may live across codec calls that can longjmp. */
void read_header(Decoder& state) {
    state.codec = png_create_read_struct(PNG_LIBPNG_VER_STRING, &state, fail, fail);
    if (!state.codec) throw PngReadError("Cannot allocate PNG reader");
    if (setjmp(png_jmpbuf(state.codec))) {
        if (state.failure) std::rethrow_exception(state.failure);
        if (state.unsupported) throw PngUnsupportedError(state.message.data());
        throw PngReadError(state.message.data());
    }
    state.info = png_create_info_struct(state.codec);
    if (!state.info) throw PngReadError("Cannot allocate PNG metadata");
    png_set_read_fn(state.codec, &state, read);
    png_set_user_limits(state.codec, 1000000, 1000000);
    png_set_chunk_malloc_max(state.codec, 16 * 1024 * 1024);
    png_set_chunk_cache_max(state.codec, 128);
    // Text and suggested palettes are irrelevant to delivery. Skip them without
    // inflating/retaining potentially large compressed ancillary payloads.
    constexpr png_byte ignored[] = {'t','E','X','t',0, 'z','T','X','t',0,
                                    'i','T','X','t',0, 's','P','L','T',0};
    png_set_keep_unknown_chunks(state.codec, PNG_HANDLE_CHUNK_NEVER, ignored, 4);
    png_set_crc_action(state.codec, PNG_CRC_ERROR_QUIT, PNG_CRC_ERROR_QUIT);
    png_set_read_user_chunk_fn(state.codec, &state, unknown_chunk);
    png_read_info(state.codec, state.info);
}
PngMetadata extract_metadata(const Decoder* state) {
    // Getters below only inspect completed metadata: no further decoding/error jumps.
    PngMetadata result;
    result.width = png_get_image_width(state->codec, state->info);
    result.height = png_get_image_height(state->codec, state->info);
    result.bit_depth = png_get_bit_depth(state->codec, state->info);
    if (result.bit_depth == 16) throw PngUnsupportedError("16-bit PNG requires ingestion normalization");
    if (png_get_valid(state->codec, state->info, PNG_INFO_eXIf))
        throw PngUnsupportedError("PNG EXIF requires ingestion normalization");
    if (png_get_valid(state->codec, state->info, PNG_INFO_cICP | PNG_INFO_cLLI | PNG_INFO_mDCV))
        throw PngUnsupportedError("PNG CICP/HDR metadata requires color management");
    switch (png_get_color_type(state->codec, state->info)) {
    case PNG_COLOR_TYPE_GRAY: result.color = PngColor::gray; break;
    case PNG_COLOR_TYPE_RGB: result.color = PngColor::rgb; break;
    case PNG_COLOR_TYPE_PALETTE: result.color = PngColor::palette; break;
    case PNG_COLOR_TYPE_GRAY_ALPHA: result.color = PngColor::gray_alpha; break;
    case PNG_COLOR_TYPE_RGBA: result.color = PngColor::rgba; break;
    default: throw PngUnsupportedError("Unsupported PNG color model");
    }
    result.interlaced = png_get_interlace_type(state->codec, state->info) == PNG_INTERLACE_ADAM7;
    result.transparency = result.color == PngColor::gray_alpha || result.color == PngColor::rgba ||
        png_get_valid(state->codec, state->info, PNG_INFO_tRNS);
    png_charp name = nullptr;
    int compression = 0;
    png_bytep profile = nullptr;
    png_uint_32 length = 0;
    if (png_get_iCCP(state->codec, state->info, &name, &compression, &profile, &length))
        result.icc_profile.assign(profile, profile + length);
    int intent = 0;
    if (result.icc_profile.empty() && png_get_sRGB(state->codec, state->info, &intent))
        result.srgb_intent = static_cast<unsigned>(intent);
    result.color_declaration = png_get_valid(state->codec, state->info,
        PNG_INFO_sRGB | PNG_INFO_gAMA | PNG_INFO_cHRM) != 0;
    return result;
}

void decode_pixels(Decoder& state, const PngMetadata& metadata) {
    if (setjmp(png_jmpbuf(state.codec))) {
        if (state.failure) std::rethrow_exception(state.failure);
        if (state.unsupported) throw PngUnsupportedError(state.message.data());
        throw PngReadError(state.message.data());
    }
    if (metadata.color == PngColor::palette) png_set_palette_to_rgb(state.codec);
    if (metadata.color == PngColor::gray && metadata.bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(state.codec);
    if (png_get_valid(state.codec, state.info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(state.codec);
    if (state.image.layout == PixelLayout::rgba &&
        (metadata.color == PngColor::gray || metadata.color == PngColor::gray_alpha))
        png_set_gray_to_rgb(state.codec);
    const int passes = png_set_interlace_handling(state.codec);
    png_read_update_info(state.codec, state.info);
    const auto row_bytes = std::size_t(state.image.width) * state.image.channel_count();
    if (png_get_bit_depth(state.codec, state.info) != 8 ||
        png_get_channels(state.codec, state.info) != state.image.channel_count() ||
        png_get_rowbytes(state.codec, state.info) != row_bytes)
        throw PngReadError("Unexpected PNG output layout");
    for (int pass = 0; pass < passes; ++pass)
        for (std::uint32_t y = 0; y < state.image.height; ++y)
            png_read_row(state.codec, state.image.pixels.data() + std::size_t(y) * row_bytes, nullptr);
    state.end = png_create_info_struct(state.codec);
    if (!state.end) throw PngReadError("Cannot allocate PNG end metadata");
    png_read_end(state.codec, state.end);
    if (png_get_valid(state.codec, state.end, PNG_INFO_eXIf))
        throw PngUnsupportedError("PNG EXIF requires ingestion normalization");
}
} // namespace

PngMetadata read_png_metadata(const ByteSource& source, std::size_t max_read_bytes) {
    const auto state = std::make_unique<Decoder>(source, max_read_bytes);
    read_header(*state);
    return extract_metadata(state.get());
}

bool supports_png_pixels(const PngMetadata& metadata) noexcept {
    return !(metadata.color_declaration && metadata.icc_profile.empty() && !metadata.srgb_intent);
}

RasterImage8 read_png_image(const ByteSource& source, const PngMetadata& metadata,
                            std::size_t max_decoded_bytes, std::size_t max_read_bytes) {
    const auto state = std::make_unique<Decoder>(source, std::min(max_read_bytes, std::size_t{16 * 1024 * 1024}));
    read_header(*state);
    auto actual = extract_metadata(state.get());
    if (actual != metadata) throw PngReadError("PNG header changed since metadata planning");
    if (!supports_png_pixels(actual)) throw PngUnsupportedError("PNG color encoding requires conversion");
    const bool gray = actual.color == PngColor::gray || actual.color == PngColor::gray_alpha;
    const auto layout = gray ? (actual.transparency ? PixelLayout::gray_alpha : PixelLayout::gray) :
                              (actual.transparency ? PixelLayout::rgba : PixelLayout::rgb);
    const std::uint64_t channels = gray ? (actual.transparency ? 2 : 1) : (actual.transparency ? 4 : 3);
    const auto pixels = std::uint64_t(actual.width) * actual.height * channels;
    if (pixels > max_decoded_bytes || actual.icc_profile.size() > max_decoded_bytes - pixels)
        throw PngReadError("PNG decoding exceeds working memory limit");
    state->image = {actual.width, actual.height, std::vector<std::uint8_t>(static_cast<std::size_t>(pixels)),
        std::move(actual.icc_profile), layout, actual.srgb_intent};
    state->remaining = max_read_bytes - static_cast<std::size_t>(state->offset);
    decode_pixels(*state, metadata);
    return std::move(state->image);
}
} // namespace sipi
