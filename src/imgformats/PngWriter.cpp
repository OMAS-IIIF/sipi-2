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

void write_png(const RasterImage8& image, const PngSink& sink) {
    constexpr std::uint64_t pixel_limit = 256 * 1024 * 1024;
    const auto channels = image.channel_count();
    const auto pixels = static_cast<std::uint64_t>(image.width) * image.height;
    if (!sink || !channels || !image.width || !image.height || image.width > 1000000 || image.height > 1000000 ||
        pixels > pixel_limit / channels || pixels * channels != image.pixels.size())
        throw PngWriteError("Invalid PNG raster or sink");
    if (image.icc_profile.size() > 16 * 1024 * 1024)
        throw PngWriteError("ICC profile exceeds PNG writer limit");

    // PNG gray/RGB images require a matching GRAY/RGB profile. Recent libpng writers check size
    // but do not fully validate ICC semantics; do not imply color validation.
    if (!image.icc_profile.empty() && (image.icc_profile.size() < 132 ||
        std::memcmp(image.icc_profile.data() + 36, "acsp", 4) != 0 ||
        std::memcmp(image.icc_profile.data() + 16, channels == 1 ? "GRAY" : "RGB ", 4) != 0))
        throw PngWriteError("PNG requires an ICC header matching its color model");

    const auto state = std::make_unique<Encoder>(sink);
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
    png_set_IHDR(state->codec, state->info, image.width, image.height, 8,
                 channels == 1 ? PNG_COLOR_TYPE_GRAY : (channels == 4 ? PNG_COLOR_TYPE_RGBA : PNG_COLOR_TYPE_RGB), PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
    if (!image.icc_profile.empty()) {
        png_set_iCCP(state->codec, state->info, "Source ICC", PNG_COMPRESSION_TYPE_BASE,
                     image.icc_profile.data(), static_cast<png_uint_32>(image.icc_profile.size()));
        if (!png_get_valid(state->codec, state->info, PNG_INFO_iCCP))
            throw PngWriteError("PNG encoder rejected ICC profile");
    }
    png_write_info(state->codec, state->info);
    for (std::uint32_t y = 0; y < image.height; ++y)
        png_write_row(state->codec, image.pixels.data() + static_cast<std::size_t>(y) * image.width * channels);
    png_write_end(state->codec, state->info);
}
} // namespace sipi
