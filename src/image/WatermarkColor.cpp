#include "WatermarkColor.h"
#include "SrgbProfiles.h"
#include <lcms2.h>

namespace sipi {
namespace {
/** Native objects must outlive transforms and their per-call error context. */
struct Conversion {
    bool failed = false;
    cmsContext context = nullptr;
    cmsHPROFILE source = nullptr, target = nullptr;
    cmsToneCurve* curve = nullptr;
    cmsHTRANSFORM transform = nullptr;
    ~Conversion() {
        if (transform) cmsDeleteTransform(transform);
        if (source) cmsCloseProfile(source);
        if (target) cmsCloseProfile(target);
        if (curve) cmsFreeToneCurve(curve);
        if (context) cmsDeleteContext(context);
    }
};
void error(cmsContext context, cmsUInt32Number, const char*) noexcept {
    static_cast<Conversion*>(cmsGetContextUserData(context))->failed = true;
}
}
std::vector<std::uint8_t> watermark_color(const RasterImage8& image,
    const std::array<std::uint8_t,3>& srgb, std::size_t limit) {
    const auto channels = image.channel_count();
    const auto count = std::uint64_t(image.width)*image.height;
    if (!channels || !count || count > limit/channels || count*channels != image.pixels.size() ||
        image.icc_profile.size() > limit-image.pixels.size() ||
        image.icc_profile.size() > 16*1024*1024 ||
        (image.srgb_intent && (*image.srgb_intent > 3 || !image.icc_profile.empty())))
        throw ColorConversionError("Invalid watermark destination raster or memory limit");
    const bool gray = channels <= 2;
    // Identical explicit/assumed sRGB encoding needs no numerical round trip.
    if (!gray && image.icc_profile.empty()) return {srgb.begin(),srgb.end()};
    std::vector<std::uint8_t> declared_profile; // Keep profile storage through native cleanup.
    Conversion state;
    state.context = cmsCreateContext(nullptr,&state);
    if (!state.context) throw ColorConversionError("Cannot allocate watermark color context");
    cmsSetLogErrorHandlerTHR(state.context,error);
    state.source = cmsCreate_sRGBProfileTHR(state.context);
    if (!state.source || state.failed) throw ColorConversionError("Cannot create watermark sRGB profile");
    if (!image.icc_profile.empty()) {
        state.target = cmsOpenProfileFromMemTHR(state.context,image.icc_profile.data(),
            static_cast<cmsUInt32Number>(image.icc_profile.size()));
    } else if (image.srgb_intent) {
        declared_profile = srgb_profile(PixelLayout::gray,*image.srgb_intent);
        state.target = cmsOpenProfileFromMemTHR(state.context,declared_profile.data(),
            static_cast<cmsUInt32Number>(declared_profile.size()));
    } else {
        state.curve = cmsBuildGamma(state.context,2.2);
        if (state.curve) state.target = cmsCreateGrayProfileTHR(state.context,cmsD50_xyY(),state.curve);
    }
    if (!state.target || state.failed ||
        cmsGetColorSpace(state.target) != (gray ? cmsSigGrayData : cmsSigRgbData) ||
        cmsGetDeviceClass(state.target) == cmsSigLinkClass ||
        cmsGetDeviceClass(state.target) == cmsSigAbstractClass ||
        cmsGetDeviceClass(state.target) == cmsSigNamedColorClass)
        throw ColorConversionError("Watermark destination profile is unusable or mismatches raster");
    state.transform = cmsCreateTransformTHR(state.context,state.source,TYPE_RGB_8,state.target,
        gray ? TYPE_GRAY_8 : TYPE_RGB_8,INTENT_RELATIVE_COLORIMETRIC,cmsFLAGS_BLACKPOINTCOMPENSATION);
    if (!state.transform || state.failed) throw ColorConversionError("Cannot build watermark color transform");
    std::vector<std::uint8_t> result(gray ? 1 : 3);
    cmsDoTransform(state.transform,srgb.data(),result.data(),1);
    if (state.failed) throw ColorConversionError("Watermark color transform failed");
    return result;
}
}
