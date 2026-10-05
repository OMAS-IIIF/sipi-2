#include "JpegWriter.h"
#include "image/SrgbProfiles.h"

#include <cstddef>
#include <cstdio>
#include <jpeglib.h>
#include <algorithm>
#include <array>
#include <csetjmp>
#include <cstring>
#include <exception>
#include <memory>

namespace sipi {
namespace {
constexpr std::size_t icc_chunk_bytes = 65533 - 14;

/** First-member layout permits recovery from libjpeg's error-manager pointer. */
struct ErrorState {
    jpeg_error_mgr manager{};
    std::jmp_buf jump;
};

/** Mutable codec state lives on the heap: longjmp must not leave modified
 * automatic variables indeterminate. The owner is created before setjmp and
 * destroyed normally afterwards. No C++ destructors are skipped by codec errors. */
struct Encoder {
    jpeg_compress_struct codec{};
    ErrorState error;
    jpeg_destination_mgr destination{};
    std::array<JOCTET, 16384> buffer{};
    std::array<JOCTET, 65533> marker{};
    const JpegSink* sink;
    std::exception_ptr sink_error;

    explicit Encoder(const JpegSink& output) : sink(&output) {}
    ~Encoder() { if (codec.mem) jpeg_destroy_compress(&codec); }
};

void fail(j_common_ptr codec) noexcept {
    auto* error = reinterpret_cast<ErrorState*>(codec->err);
    std::longjmp(error->jump, 1);
}
void silence(j_common_ptr) noexcept {}

/** Fully unwind a throwing sink before jumping back out of the C codec. */
void emit(Encoder& state, std::size_t count) noexcept {
    try {
        (*state.sink)({state.buffer.data(), count});
    } catch (...) {
        state.sink_error = std::current_exception();
    }
    if (state.sink_error) std::longjmp(state.error.jump, 1);
}
void start_destination(j_compress_ptr codec) noexcept {
    auto& state = *static_cast<Encoder*>(codec->client_data);
    state.destination.next_output_byte = state.buffer.data();
    state.destination.free_in_buffer = state.buffer.size();
}
boolean flush_destination(j_compress_ptr codec) noexcept {
    auto& state = *static_cast<Encoder*>(codec->client_data);
    emit(state, state.buffer.size());
    start_destination(codec);
    return TRUE;
}
void finish_destination(j_compress_ptr codec) noexcept {
    auto& state = *static_cast<Encoder*>(codec->client_data);
    const auto count = state.buffer.size() - state.destination.free_in_buffer;
    if (count) emit(state, count);
}
} // namespace

void write_jpeg(const RasterImage8& image, const JpegSink& sink, int quality) {
    const auto channels = image.channel_count();
    if ((image.layout != PixelLayout::rgb && image.layout != PixelLayout::gray) || !sink || quality < 1 || quality > 100 || image.width == 0 || image.height == 0 ||
        image.width > JPEG_MAX_DIMENSION || image.height > JPEG_MAX_DIMENSION ||
        static_cast<std::uint64_t>(image.width) * image.height * channels != image.pixels.size())
        throw JpegWriteError("Invalid JPEG raster, quality or sink");
    if (image.icc_profile.size() > 255 * icc_chunk_bytes)
        throw JpegWriteError("ICC profile exceeds JPEG marker capacity");

    if (image.layout == PixelLayout::gray && !image.icc_profile.empty() &&
        (image.icc_profile.size() < 132 || std::memcmp(image.icc_profile.data()+36, "acsp", 4) != 0 ||
         std::memcmp(image.icc_profile.data()+16, "GRAY", 4) != 0))
        throw JpegWriteError("Grayscale JPEG requires a gray ICC profile header");

    if (image.srgb_intent && (*image.srgb_intent > 3 || !image.icc_profile.empty()))
        throw JpegWriteError("sRGB declaration must be valid and mutually exclusive with ICC");
    // Both owning objects precede setjmp; codec errors cannot skip their destruction.
    const auto assigned = image.srgb_intent ? srgb_profile(image.layout, *image.srgb_intent) :
                                            std::vector<std::uint8_t>{};
    const auto& profile = image.srgb_intent ? assigned : image.icc_profile;
    const auto state = std::make_unique<Encoder>(sink);
    state->codec.err = jpeg_std_error(&state->error.manager);
    state->error.manager.error_exit = fail;
    state->error.manager.output_message = silence;
    if (setjmp(state->error.jump)) {
        if (state->sink_error) std::rethrow_exception(state->sink_error);
        throw JpegWriteError("JPEG encoding failed");
    }
    jpeg_create_compress(&state->codec);
    state->codec.client_data = state.get();
    state->destination.init_destination = start_destination;
    state->destination.empty_output_buffer = flush_destination;
    state->destination.term_destination = finish_destination;
    state->codec.dest = &state->destination;
    state->codec.image_width = image.width;
    state->codec.image_height = image.height;
    state->codec.input_components = static_cast<int>(channels);
    state->codec.in_color_space = channels == 1 ? JCS_GRAYSCALE : JCS_RGB;
    jpeg_set_defaults(&state->codec);
    jpeg_set_quality(&state->codec, quality, TRUE);
    for (int i = 0; i < state->codec.num_components; ++i) {
        state->codec.comp_info[i].h_samp_factor = 1;
        state->codec.comp_info[i].v_samp_factor = 1;
    }
    jpeg_start_compress(&state->codec, TRUE);
    // ICC APP2 payload: signature including NUL, sequence number, chunk count.
    std::memcpy(state->marker.data(), "ICC_PROFILE\0", 12);
    state->marker[13] = static_cast<JOCTET>((profile.size() + icc_chunk_bytes - 1) / icc_chunk_bytes);
    for (std::size_t offset = 0; offset < profile.size(); offset += icc_chunk_bytes) {
        const auto count = std::min(icc_chunk_bytes, profile.size() - offset);
        state->marker[12] = static_cast<JOCTET>(offset / icc_chunk_bytes + 1);
        std::memcpy(state->marker.data() + 14, profile.data() + offset, count);
        jpeg_write_marker(&state->codec, JPEG_APP0 + 2, state->marker.data(), static_cast<unsigned>(count + 14));
    }
    while (state->codec.next_scanline < image.height) {
        // libjpeg's API predates const-correctness; input scanlines are not modified.
        auto* row = const_cast<JSAMPLE*>(image.pixels.data() +
            static_cast<std::size_t>(state->codec.next_scanline) * image.width * channels);
        jpeg_write_scanlines(&state->codec, &row, 1);
    }
    jpeg_finish_compress(&state->codec);
}
} // namespace sipi
