#include "J2kReader.h"
#include "J2kReadPlan.h"
#include <cstring>
#include <grok.h>
#include <algorithm>
#include <exception>
#include <limits>
#include <memory>

namespace sipi {
namespace {
// Grok's handlers are process-global. Install once; synchronous codec-local
// execution lets each request capture diagnostics through its own thread scope.
// Warnings include incomplete tile decoding even when grk_decompress returns true.
thread_local bool* current_diagnostic = nullptr;
void ignore_message(const char*, void*) noexcept {}
void fail_message(const char*, void*) noexcept {
    if (current_diagnostic) *current_diagnostic = true;
}
void warn_message(const char* message, void* context) noexcept {
    // JP2 is extensible: Grok deliberately skips unrecognized boxes (e.g. UUID
    // information, uinf). This pinned codec diagnostic reports metadata skipping,
    // not missing pixels. All other warnings, and ALL errors, remain fatal.
    constexpr char skipped_box[] = "Ignoring unknown box of type ";
    if (message && std::strncmp(message, skipped_box, sizeof(skipped_box)-1) == 0) return;
    fail_message(message, context);
}
struct Diagnostics {
    bool failed = false;
    bool* previous = current_diagnostic;
    Diagnostics() { current_diagnostic = &failed; }
    ~Diagnostics() { current_diagnostic = previous; }
    void check() const { if (failed) throw J2kReadError("JPEG 2000 codec reported invalid or incomplete data"); }
};
struct Runtime {
    Runtime() {
        grk_msg_handlers handlers{};
        handlers.info_callback = ignore_message;
        handlers.warn_callback = warn_message;
        handlers.error_callback = fail_message;
        grk_set_msg_handlers(handlers);
        grk_initialize(nullptr, 1, nullptr);
    }
    ~Runtime() { grk_deinitialize(); }
};

/** Exceptions cannot cross the codec's C callback boundary. Preserve the first
 * failure and rethrow it after the codec returns, including source-version errors. */
struct Input {
    const ByteSource& source;
    std::size_t remaining;
    std::uint64_t offset = 0;
    std::exception_ptr failure;

    static std::size_t read(std::uint8_t* data, std::size_t size, void* opaque) noexcept {
        auto& self = *static_cast<Input*>(opaque);
        if (self.failure) return 0;
        try {
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(
                size, self.source.info().size - self.offset));
            if (count > self.remaining) throw J2kReadError("JPEG 2000 input read limit exceeded");
            const auto read = self.source.read_at(self.offset,
                std::as_writable_bytes(std::span{data, count}));
            self.offset += read;
            self.remaining -= read;
            return read;
        } catch (...) {
            self.failure = std::current_exception();
            return 0;
        }
    }
    static bool seek(std::uint64_t offset, void* opaque) noexcept {
        auto& self = *static_cast<Input*>(opaque);
        if (self.failure || offset > self.source.info().size) return false;
        self.offset = offset;
        return true;
    }
    void check() const { if (failure) std::rethrow_exception(failure); }
};
using Codec = std::unique_ptr<grk_object, decltype(&grk_object_unref)>;

Codec open(Input& input, grk_decompress_parameters& parameters) {
    static Runtime runtime;
    const auto size = input.source.info().size;
    if (!size || size > std::numeric_limits<std::size_t>::max())
        throw J2kReadError("Invalid JPEG 2000 source size");
    grk_stream_params stream{};
    stream.read_fn = Input::read;
    stream.seek_fn = Input::seek;
    stream.user_data = &input;
    stream.stream_len = static_cast<std::size_t>(size);
    stream.is_read_stream = true;
    stream.double_buffer_len = 64 * 1024;
    stream.initial_double_buffer_len = 64 * 1024;
    parameters.num_threads = 1; // Codec-local inline executor, no callback races.
    Codec codec(grk_decompress_init(&stream, &parameters), grk_object_unref);
    input.check();
    if (!codec) throw J2kReadError("Cannot open JPEG 2000 stream");
    return codec;
}

J2kMetadata extract_metadata(const grk_header_info& header) {
    const auto& image = header.header_image;
    if (image.x1 <= image.x0 || image.y1 <= image.y0 || !image.numcomps || !image.comps)
        throw J2kReadError("Invalid JPEG 2000 dimensions or components");
    J2kMetadata result{image.x1 - image.x0, image.y1 - image.y0,
        image.x0, image.y0, header.t_width, header.t_height, header.numresolutions, {}, {}};
    result.components.reserve(image.numcomps);
    for (std::uint16_t i = 0; i < image.numcomps; ++i) {
        const auto& c = image.comps[i];
        result.components.push_back({c.w, c.h, c.prec, c.dx, c.dy, c.sgnd});
    }
    if (image.meta && image.meta->color.icc_profile_len) {
        const auto& color = image.meta->color;
        if (!color.icc_profile_buf || color.icc_profile_len > 16 * 1024 * 1024)
            throw J2kReadError("Invalid or excessive JPEG 2000 ICC profile");
        result.icc_profile.assign(color.icc_profile_buf, color.icc_profile_buf + color.icc_profile_len);
    }
    result.tile_origin_x = header.tx0;
    result.tile_origin_y = header.ty0;
    result.mapped_channels = image.meta &&
        (image.meta->color.palette || image.meta->color.channel_definition);
    if (!result.icc_profile.empty()) {
        if (result.icc_profile.size() < 132 ||
            std::memcmp(result.icc_profile.data()+36,"acsp",4) != 0)
            result.color = J2kColor::unsupported;
        else if (std::memcmp(result.icc_profile.data()+16,"RGB ",4) == 0)
            result.color = J2kColor::rgb;
        else if (std::memcmp(result.icc_profile.data()+16,"GRAY",4) == 0)
            result.color = J2kColor::gray;
        else result.color = J2kColor::unsupported;
    } else if (image.color_space == GRK_CLRSPC_SRGB) result.color = J2kColor::rgb;
    else if (image.color_space == GRK_CLRSPC_GRAY) result.color = J2kColor::gray;
    else if (image.color_space == GRK_CLRSPC_UNKNOWN)
        result.color = image.numcomps == 1 ? J2kColor::gray : J2kColor::unknown;
    else result.color = J2kColor::unsupported;
    return result;
}

J2kMetadata read_header(grk_object* codec, Input& input) {
    grk_header_info header{};
    // Unknown output format requests raw components: no implicit ICC conversion.
    const auto success = grk_decompress_read_header(codec, &header);
    input.check();
    if (!success) throw J2kReadError("Cannot read JPEG 2000 header");
    return extract_metadata(header);
}
} // namespace

J2kMetadata read_j2k_metadata(const ByteSource& source, std::size_t max_read_bytes) {
    Diagnostics diagnostics;
    Input input{source, max_read_bytes, 0, {}};
    grk_decompress_parameters parameters{};
    parameters.core.skip_allocate_composite = true;
    auto codec = open(input, parameters);
    auto result = read_header(codec.get(), input);
    diagnostics.check();
    return result;
}

bool supports_j2k_pixels(const J2kMetadata& m) noexcept {
    const auto channels = m.color == J2kColor::gray ? 1u : m.color == J2kColor::rgb ? 3u : 0u;
    return channels && m.components.size() == channels && !m.mapped_channels &&
        m.tile_width && m.tile_height && m.resolution_count &&
        std::all_of(m.components.begin(),m.components.end(),[&](const auto& c) {
            return c.width == m.width && c.height == m.height && c.precision == 8 &&
                   c.step_x == 1 && c.step_y == 1 && !c.is_signed;
        });
}

RasterImage8 read_j2k_region(const ByteSource& source, const J2kMetadata& metadata,
                           const J2kReadPlan& plan, std::size_t budget) {
    if (!supports_j2k_pixels(metadata))
        throw J2kUnsupportedError("Unsupported JPEG 2000 color/sample layout");
    const auto& r = plan.source_region;
    const auto& out = plan.reduced_region;
    using Wide = std::uint64_t;
    if (plan.reduction >= metadata.resolution_count || plan.reduction >= 32 ||
        !r.width || !r.height || Wide{r.x}+r.width > metadata.width ||
        Wide{r.y}+r.height > metadata.height || !out.width || !out.height)
        throw J2kReadError("Invalid JPEG 2000 read plan");
    const Wide channels = metadata.components.size();
    const Wide output_area = Wide{out.width}*out.height;
    if (output_area > budget/channels) throw J2kReadError("JPEG 2000 output exceeds budget");
    const Wide output_bytes = output_area*channels;
    // Bound the selected tiles conservatively, allowing all their reduced int32
    // samples to coexist with our packed output. Codec scratch is additional.
    const Wide step = Wide{1}<<plan.reduction;
    const auto tile_span = [](Wide first, Wide extent, Wide origin, Wide tile) {
        return (first+extent-1-origin)/tile - (first-origin)/tile + 1;
    };
    if (metadata.tile_origin_x > metadata.origin_x || metadata.tile_origin_y > metadata.origin_y)
        throw J2kReadError("Invalid JPEG 2000 tile origin");
    const Wide nx = tile_span(Wide{metadata.origin_x}+r.x,r.width,metadata.tile_origin_x,metadata.tile_width);
    const Wide ny = tile_span(Wide{metadata.origin_y}+r.y,r.height,metadata.tile_origin_y,metadata.tile_height);
    const Wide tw = (Wide{metadata.tile_width}+step-1)/step;
    const Wide th = (Wide{metadata.tile_height}+step-1)/step;
    Wide allowance = (budget-output_bytes)/(channels*sizeof(std::int32_t));
    for (const Wide factor : {nx,ny,tw,th}) {
        if (!factor || factor > allowance) throw J2kReadError("JPEG 2000 tile samples exceed budget");
        allowance /= factor;
    }
    Diagnostics diagnostics;
    Input input{source, 256*1024*1024, 0, {}};
    grk_decompress_parameters parameters{};
    parameters.core.reduce = plan.reduction;
    // The all-zero window means the whole image, avoiding Grok's <=1 fraction
    // interpretation for a genuine 1x1 source. Other tiny windows are enlarged
    // by the planner and cropped exactly by the shared resampler afterwards.
    if (r.x || r.y || r.width != metadata.width || r.height != metadata.height) {
        parameters.dw_x0 = r.x; parameters.dw_y0 = r.y;
        parameters.dw_x1 = Wide{r.x}+r.width; parameters.dw_y1 = Wide{r.y}+r.height;
    }
    auto codec = open(input,parameters);
    const auto actual = read_header(codec.get(),input);
    diagnostics.check();
    if (actual.width != metadata.width || actual.height != metadata.height ||
        actual.origin_x != metadata.origin_x || actual.origin_y != metadata.origin_y ||
        actual.color != metadata.color || actual.icc_profile != metadata.icc_profile ||
        actual.mapped_channels || actual.components.size() != metadata.components.size() ||
        actual.tile_width != metadata.tile_width || actual.tile_height != metadata.tile_height ||
        actual.tile_origin_x != metadata.tile_origin_x || actual.tile_origin_y != metadata.tile_origin_y ||
        !std::equal(actual.components.begin(), actual.components.end(), metadata.components.begin(),
            [](const auto& a, const auto& b) {
                return a.precision == b.precision && a.is_signed == b.is_signed &&
                       a.step_x == b.step_x && a.step_y == b.step_y;
            }))
        throw J2kReadError("JPEG 2000 source differs from metadata");
    const bool success = grk_decompress(codec.get(),nullptr);
    input.check();
    diagnostics.check();
    if (!success) throw J2kReadError("JPEG 2000 pixel decoding failed");
    const auto* image = grk_decompress_get_image(codec.get());
    if (!image || image->numcomps != channels || !image->comps)
        throw J2kReadError("Missing JPEG 2000 decoded components");
    RasterImage8 result{out.width,out.height,std::vector<std::uint8_t>(output_bytes),
        actual.icc_profile, channels == 1 ? PixelLayout::gray : PixelLayout::rgb};
    for (std::size_t c=0; c<channels; ++c) {
        const auto& component = image->comps[c];
        if (!component.data || component.w != out.width || component.h != out.height ||
            component.stride < component.w ||
            (component.data_type != GRK_INT_16 && component.data_type != GRK_INT_32))
            throw J2kReadError("Unexpected JPEG 2000 decoded layout");
        for (std::size_t y=0; y<out.height; ++y) {
            for (std::size_t x=0; x<out.width; ++x) {
                const auto index = y*component.stride+x;
                const auto value = component.data_type == GRK_INT_16 ?
                    static_cast<const std::int16_t*>(component.data)[index] :
                    static_cast<const std::int32_t*>(component.data)[index];
                if (value < 0 || value > 255) throw J2kReadError("Invalid JPEG 2000 sample value");
                result.pixels[(y*out.width+x)*channels+c] = static_cast<std::uint8_t>(value);
            }
        }
    }
    return result;
}
} // namespace sipi
