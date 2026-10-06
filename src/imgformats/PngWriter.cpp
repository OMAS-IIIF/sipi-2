#include "PngWriter.h"

#include <png.h>
#include <algorithm>
#include <csetjmp>
#include <cstring>
#include <exception>
#include <memory>

namespace sipi {
namespace {
/** Heap state survives longjmp without indeterminate modified local variables.
 * Its owner exists before setjmp; no nontrivial C++ frames are skipped. */
struct Encoder {
    png_structp codec = nullptr;
    png_infop info = nullptr;
    const PngSink* sink;
    std::exception_ptr sink_error;
    std::vector<std::uint8_t> packed_row; // Owned before setjmp; safe across codec failures.

    explicit Encoder(const PngSink& output) : sink(&output) {}
    ~Encoder() { if (codec) png_destroy_write_struct(&codec, &info); }
};

void fail(png_structp codec, png_const_charp) noexcept { png_longjmp(codec, 1); }

/** Unwind the sink completely before returning through libpng's jump target. */
void emit(png_structp codec, png_bytep bytes, png_size_t count) noexcept {
    auto& state = *static_cast<Encoder*>(png_get_io_ptr(codec));
    try {
        while (count) {
            const auto block = std::min<png_size_t>(count, 16384);
            (*state.sink)({bytes, block});
            bytes += block;
            count -= block;
        }
    } catch (...) {
        state.sink_error = std::current_exception();
    }
    if (state.sink_error) png_longjmp(codec, 1);
}
void flush(png_structp) noexcept {} // The synchronous sink owns persistence/flush policy.
} // namespace

void write_png(const RasterImage8& image, const PngSink& sink, PngBitDepth depth) {
    constexpr std::uint64_t pixel_limit = 256 * 1024 * 1024;
    const auto channels = image.channel_count();
    const auto pixels = static_cast<std::uint64_t>(image.width) * image.height;
    if (!sink || !channels || !image.width || !image.height || image.width > 1000000 || image.height > 1000000 ||
        pixels > pixel_limit / channels || pixels * channels != image.pixels.size())
        throw PngWriteError("Invalid PNG raster or sink");
    if (depth != PngBitDepth::eight && depth != PngBitDepth::one)
        throw PngWriteError("Invalid PNG bit depth");
    if (depth == PngBitDepth::one &&
        (image.layout != PixelLayout::gray ||
         std::any_of(image.pixels.begin(), image.pixels.end(),
                     [](auto sample) { return sample != 0 && sample != 255; })))
        throw PngWriteError("One-bit PNG requires opaque gray samples of 0 or 255");
    if (image.icc_profile.size() > 16 * 1024 * 1024)
        throw PngWriteError("ICC profile exceeds PNG writer limit");

    // PNG gray/RGB images require a matching GRAY/RGB profile. Recent libpng writers check size
    // but do not fully validate ICC semantics; do not imply color validation.
    if (!image.icc_profile.empty() && (image.icc_profile.size() < 132 ||
        std::memcmp(image.icc_profile.data() + 36, "acsp", 4) != 0 ||
        std::memcmp(image.icc_profile.data() + 16, channels <= 2 ? "GRAY" : "RGB ", 4) != 0))
        throw PngWriteError("PNG requires an ICC header matching its color model");

    if (image.srgb_intent && (*image.srgb_intent > 3 || !image.icc_profile.empty()))
        throw PngWriteError("sRGB declaration must be valid and mutually exclusive with ICC");
    const auto state = std::make_unique<Encoder>(sink);
    if (depth == PngBitDepth::one) state->packed_row.resize((image.width + 7) / 8);
    // Treat warnings as failures too: an invalid profile must not disappear.
    state->codec = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, fail, fail);
    if (!state->codec) throw PngWriteError("Cannot allocate PNG encoder");
    if (setjmp(png_jmpbuf(state->codec))) {
        if (state->sink_error) std::rethrow_exception(state->sink_error);
        throw PngWriteError("PNG encoding failed");
    }
    state->info = png_create_info_struct(state->codec);
    if (!state->info) throw PngWriteError("Cannot allocate PNG metadata");
    png_set_write_fn(state->codec, state.get(), emit, flush);
    png_set_compression_level(state->codec, 6);
    png_set_IHDR(state->codec, state->info, image.width, image.height, depth == PngBitDepth::one ? 1 : 8,
                 channels == 1 ? PNG_COLOR_TYPE_GRAY : channels == 2 ? PNG_COLOR_TYPE_GRAY_ALPHA : (channels == 4 ? PNG_COLOR_TYPE_RGBA : PNG_COLOR_TYPE_RGB), PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
    if (!image.icc_profile.empty()) {
        png_set_iCCP(state->codec, state->info, "Source ICC", PNG_COMPRESSION_TYPE_BASE,
                     image.icc_profile.data(), static_cast<png_uint_32>(image.icc_profile.size()));
        if (!png_get_valid(state->codec, state->info, PNG_INFO_iCCP))
            throw PngWriteError("PNG encoder rejected ICC profile");
    }
    if (image.srgb_intent)
        png_set_sRGB(state->codec, state->info, static_cast<int>(*image.srgb_intent));
    png_write_info(state->codec, state->info);
    for (std::uint32_t y = 0; y < image.height; ++y) {
        const auto* row = image.pixels.data() + static_cast<std::size_t>(y) * image.width * channels;
        if (depth == PngBitDepth::one) {
            std::fill(state->packed_row.begin(), state->packed_row.end(), 0);
            for (std::uint32_t x = 0; x < image.width; ++x) {
                // PNG stores the leftmost sample in the most significant bit.
                if (row[x] == 255)
                    state->packed_row[x / 8] |= static_cast<std::uint8_t>(0x80u >> (x % 8));
            }
            png_write_row(state->codec, state->packed_row.data());
        } else {
            png_write_row(state->codec, row);
        }
    }
    png_write_end(state->codec, state->info);
}
} // namespace sipi
