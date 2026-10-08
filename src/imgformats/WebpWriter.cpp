#include "WebpWriter.h"
#include "image/SrgbProfiles.h"
#include <webp/encode.h>
#include <webp/mux.h>
#include <lcms2.h>
#include <algorithm>
#include <cstring>
#include <exception>
#include <memory>

namespace sipi {
namespace {
constexpr std::size_t encoded_limit = 64 * 1024 * 1024;
/** All C allocations have one owner; callbacks never unwind across codec frames. */
struct Encoder {
    WebPPicture picture{};
    WebPData assembled{};
    std::vector<std::uint8_t> encoded;
    std::exception_ptr callback_error;
    cmsContext context = nullptr;
    cmsHPROFILE source = nullptr, target = nullptr;
    cmsToneCurve* curve = nullptr;
    cmsHTRANSFORM transform = nullptr;
    bool color_error = false;
    ~Encoder() {
        WebPPictureFree(&picture);
        WebPDataClear(&assembled);
        if (transform) cmsDeleteTransform(transform);
        if (source) cmsCloseProfile(source);
        if (target) cmsCloseProfile(target);
        if (curve) cmsFreeToneCurve(curve);
        if (context) cmsDeleteContext(context);
    }
};
int collect(const std::uint8_t* data, std::size_t size, const WebPPicture* picture) noexcept {
    auto& state = *static_cast<Encoder*>(picture->custom_ptr);
    try {
        if (size > encoded_limit - state.encoded.size()) throw WebpWriteError("WebP exceeds response limit");
        state.encoded.insert(state.encoded.end(), data, data + size);
        return 1;
    } catch (...) { state.callback_error = std::current_exception(); return 0; }
}
void color_error(cmsContext context, cmsUInt32Number, const char*) noexcept {
    static_cast<Encoder*>(cmsGetContextUserData(context))->color_error = true;
}
}
void write_webp(const RasterImage8& image, const WebpSink& sink, bool lossless, int quality) {
    constexpr std::size_t working_limit = 256 * 1024 * 1024 - 128 * 1024; // Row/profiles reserve.
    const auto channels = image.channel_count();
    const auto count = std::uint64_t{image.width} * image.height;
    if (!sink || !channels || !count || image.width > webp_maximum_dimension ||
        image.height > webp_maximum_dimension || quality < 1 || quality > 100 ||
        count > working_limit / (channels + 4) || image.pixels.size() != count * channels ||
        image.icc_profile.size() > 16 * 1024 * 1024 ||
        image.icc_profile.size() > working_limit - count * (channels + 4) ||
        (image.srgb_intent && (*image.srgb_intent > 3 || !image.icc_profile.empty())))
        throw WebpWriteError("Invalid WebP input, options or working budget");
    const bool gray = channels <= 2;
    const bool alpha = channels == 2 || channels == 4;
    if (!image.icc_profile.empty() && (image.icc_profile.size() < 132 ||
        std::memcmp(image.icc_profile.data()+36,"acsp",4) != 0 ||
        std::memcmp(image.icc_profile.data()+16,gray ? "GRAY" : "RGB ",4) != 0))
        throw WebpWriteError("WebP source ICC does not match raster");
    Encoder state;
    // The output profile is borrowed from the input unless generated below.
    std::vector<std::uint8_t> generated;
    std::span<const std::uint8_t> profile = image.icc_profile;
    std::vector<std::uint8_t> declared_gray;
    if (gray) {
        state.context = cmsCreateContext(nullptr, &state);
        if (!state.context) throw WebpWriteError("Cannot allocate WebP color context");
        cmsSetLogErrorHandlerTHR(state.context, color_error);
        auto source_profile = profile;
        if (image.srgb_intent) {
            declared_gray = srgb_profile(PixelLayout::gray, *image.srgb_intent);
            source_profile = declared_gray;
        }
        if (!source_profile.empty()) {
            state.source = cmsOpenProfileFromMemTHR(state.context, source_profile.data(),
                                                    static_cast<cmsUInt32Number>(source_profile.size()));
        } else {
            state.curve = cmsBuildGamma(state.context, 2.2);
            if (state.curve) state.source = cmsCreateGrayProfileTHR(state.context, cmsD50_xyY(), state.curve);
        }
        if (!state.source || state.color_error || cmsGetColorSpace(state.source) != cmsSigGrayData ||
            cmsGetDeviceClass(state.source) == cmsSigLinkClass ||
            cmsGetDeviceClass(state.source) == cmsSigAbstractClass ||
            cmsGetDeviceClass(state.source) == cmsSigNamedColorClass)
            throw WebpWriteError("WebP source gray profile is unusable");
        generated = srgb_profile(PixelLayout::rgb, INTENT_RELATIVE_COLORIMETRIC);
        profile = generated;
        state.target = cmsOpenProfileFromMemTHR(state.context, generated.data(),
                                                static_cast<cmsUInt32Number>(generated.size()));
        if (!state.target) throw WebpWriteError("Cannot open WebP target profile");
        state.transform = cmsCreateTransformTHR(state.context, state.source,
            alpha ? TYPE_GRAYA_8 : TYPE_GRAY_8, state.target, alpha ? TYPE_RGBA_8 : TYPE_RGB_8,
            INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_BLACKPOINTCOMPENSATION | (alpha ? cmsFLAGS_COPY_ALPHA : 0));
        if (!state.transform || state.color_error) throw WebpWriteError("Cannot convert WebP gray to sRGB");
    } else if (image.srgb_intent) {
        generated = srgb_profile(PixelLayout::rgb, *image.srgb_intent);
        profile = generated;
    }
    if (!WebPPictureInit(&state.picture)) throw WebpWriteError("WebP ABI mismatch");
    state.picture.width = static_cast<int>(image.width);
    state.picture.height = static_cast<int>(image.height);
    state.picture.use_argb = 1;
    state.picture.writer = collect;
    state.picture.custom_ptr = &state;
    if (!WebPPictureAlloc(&state.picture)) throw WebpWriteError("Cannot allocate WebP raster");
    const std::size_t output_channels = alpha ? 4 : 3;
    std::vector<std::uint8_t> row(gray ? image.width * output_channels : 0);
    for (std::uint32_t y = 0; y < image.height; ++y) {
        const auto* input = image.pixels.data() + std::size_t(y) * image.width * channels;
        if (gray) {
            cmsDoTransform(state.transform, input, row.data(), image.width);
            if (state.color_error) throw WebpWriteError("WebP color conversion failed");
            input = row.data();
        }
        auto* destination = state.picture.argb + std::size_t(y) * state.picture.argb_stride;
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const auto* pixel = input + std::size_t(x) * output_channels;
            destination[x] = (std::uint32_t(alpha ? pixel[3] : 255) << 24) |
                (std::uint32_t(pixel[0]) << 16) | (std::uint32_t(pixel[1]) << 8) | pixel[2];
        }
    }
    WebPConfig config;
    if (!WebPConfigInit(&config)) throw WebpWriteError("WebP config ABI mismatch");
    config.lossless = lossless;
    config.quality = static_cast<float>(quality);
    config.method = 4;
    config.exact = 1;
    config.thread_level = 0; // The application already bounds concurrency.
    if (!WebPValidateConfig(&config) || !WebPEncode(&config, &state.picture)) {
        if (state.callback_error) std::rethrow_exception(state.callback_error);
        throw WebpWriteError("WebP encoding failed");
    }
    if (profile.size() + 64 > encoded_limit - state.encoded.size())
        throw WebpWriteError("WebP profile exceeds mux budget");
    const std::unique_ptr<WebPMux, decltype(&WebPMuxDelete)> mux(WebPMuxNew(), WebPMuxDelete);
    const WebPData bitstream{state.encoded.data(), state.encoded.size()};
    if (!mux || WebPMuxSetImage(mux.get(), &bitstream, 0) != WEBP_MUX_OK)
        throw WebpWriteError("Cannot assemble WebP");
    if (!profile.empty()) {
        const WebPData icc{profile.data(), profile.size()};
        if (WebPMuxSetChunk(mux.get(), "ICCP", &icc, 0) != WEBP_MUX_OK)
            throw WebpWriteError("Cannot attach WebP ICC");
    }
    if (WebPMuxAssemble(mux.get(), &state.assembled) != WEBP_MUX_OK || state.assembled.size > encoded_limit)
        throw WebpWriteError("WebP assembly exceeds response limit or failed");
    for (std::size_t offset = 0; offset < state.assembled.size;) {
        const auto size = std::min<std::size_t>(16384, state.assembled.size - offset);
        sink({state.assembled.bytes + offset, size});
        offset += size;
    }
}
}
