#include "GrayConversion.h"
#include <lcms2.h>
#include <algorithm>
#include <array>

namespace sipi {
namespace {
/** Per-call context and native resources, destroyed in dependency order. */
struct Conversion {
    bool failed = false;
    cmsContext context = nullptr;
    cmsHPROFILE source = nullptr, target = nullptr;
    cmsToneCurve* curve = nullptr;
    cmsMLU* description = nullptr;
    cmsHTRANSFORM transform = nullptr;
    ~Conversion() {
        if (transform) cmsDeleteTransform(transform);
        if (source) cmsCloseProfile(source);
        if (target) cmsCloseProfile(target);
        if (curve) cmsFreeToneCurve(curve);
        if (description) cmsMLUfree(description);
        if (context) cmsDeleteContext(context);
    }
};
void error(cmsContext context, cmsUInt32Number, const char*) noexcept {
    static_cast<Conversion*>(cmsGetContextUserData(context))->failed = true;
}
}
RasterImage8 convert_to_gray(RasterImage8 image, std::size_t limit) {
    const auto channels = image.channel_count();
    const auto count = std::uint64_t(image.width)*image.height;
    if (!channels || !count || count > limit/channels || count*channels != image.pixels.size() ||
        image.icc_profile.size() > limit-image.pixels.size() ||
        (image.srgb_intent && (*image.srgb_intent > 3 || !image.icc_profile.empty())))
        throw ColorConversionError("Invalid color raster or working memory limit");
    if (image.layout == PixelLayout::gray || image.layout == PixelLayout::gray_alpha) return image;
    if (image.icc_profile.size() > 16*1024*1024)
        throw ColorConversionError("Source ICC exceeds color conversion limit");
    Conversion state;
    state.context = cmsCreateContext(nullptr,&state);
    if (!state.context) throw ColorConversionError("Cannot allocate color context");
    cmsSetLogErrorHandlerTHR(state.context,error);
    state.source = image.icc_profile.empty() ? cmsCreate_sRGBProfileTHR(state.context) :
        cmsOpenProfileFromMemTHR(state.context,image.icc_profile.data(),
                                static_cast<cmsUInt32Number>(image.icc_profile.size()));
    if (!state.source || state.failed || cmsGetColorSpace(state.source) != cmsSigRgbData ||
        cmsGetDeviceClass(state.source) == cmsSigLinkClass ||
        cmsGetDeviceClass(state.source) == cmsSigAbstractClass ||
        cmsGetDeviceClass(state.source) == cmsSigNamedColorClass)
        throw ColorConversionError("Source profile is not a usable RGB ICC profile");
    state.curve = cmsBuildGamma(state.context,2.2);
    if (!state.curve) throw ColorConversionError("Cannot allocate gray tone curve");
    state.target = cmsCreateGrayProfileTHR(state.context,cmsD50_xyY(),state.curve);
    if (!state.target || state.failed) throw ColorConversionError("Cannot create target gray profile");
    state.description = cmsMLUalloc(state.context,1);
    if (!state.description || !cmsMLUsetASCII(state.description,"en","US","sipi-2 Gray Gamma 2.2 D50") ||
        !cmsWriteTag(state.target,cmsSigProfileDescriptionTag,state.description))
        throw ColorConversionError("Cannot describe target gray profile");
    cmsSetProfileVersion(state.target,4.3);
    cmsSetHeaderRenderingIntent(state.target,INTENT_RELATIVE_COLORIMETRIC);
    cmsUInt32Number profile_size = 0;
    if (!cmsSaveProfileToMem(state.target,nullptr,&profile_size) || state.failed || profile_size < 128)
        throw ColorConversionError("Cannot size target profile");
    const std::size_t output_channels = channels == 4 ? 2 : 1;
    const auto available = limit-image.pixels.size()-image.icc_profile.size();
    if (profile_size > available || count > (available-profile_size)/output_channels)
        throw ColorConversionError("Gray output exceeds working memory limit");
    RasterImage8 output{image.width,image.height,
        std::vector<std::uint8_t>(static_cast<std::size_t>(count)*output_channels),
        std::vector<std::uint8_t>(profile_size),
        channels == 4 ? PixelLayout::gray_alpha : PixelLayout::gray};
    if (!cmsSaveProfileToMem(state.target,output.icc_profile.data(),&profile_size) || state.failed)
        throw ColorConversionError("Cannot serialize target profile");
    // Built-in profiles carry their creation time. Fix it and unset the optional
    // profile ID so independently rendered responses/cache entries are stable.
    constexpr std::array<std::uint8_t,12> date{7,234,0,1,0,1,0,0,0,0,0,0}; // 2026-01-01 UTC
    std::copy(date.begin(),date.end(),output.icc_profile.begin()+24);
    std::fill(output.icc_profile.begin()+84,output.icc_profile.begin()+100,0);
    state.transform = cmsCreateTransformTHR(state.context,state.source,
        channels == 4 ? TYPE_RGBA_8 : TYPE_RGB_8,state.target,
        channels == 4 ? TYPE_GRAYA_8 : TYPE_GRAY_8,INTENT_RELATIVE_COLORIMETRIC,
        cmsFLAGS_BLACKPOINTCOMPENSATION | (channels == 4 ? cmsFLAGS_COPY_ALPHA : 0));
    if (!state.transform || state.failed) throw ColorConversionError("Cannot build RGB-to-gray transform");
    for (std::uint32_t y=0; y<image.height; ++y) {
        cmsDoTransform(state.transform,image.pixels.data()+std::size_t(y)*image.width*channels,
            output.pixels.data()+std::size_t(y)*image.width*output_channels,image.width);
        if (state.failed) throw ColorConversionError("RGB-to-gray transform failed");
    }
    return output;
}
}
