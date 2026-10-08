#include "Watermark.h"
#include <algorithm>
#include <cmath>
#include <utility>

namespace sipi {
namespace {
void validate_mask(const RasterImage8& mask, std::size_t budget) {
    const auto count = static_cast<std::uint64_t>(mask.width) * mask.height;
    if (!mask.width || !mask.height || mask.layout != PixelLayout::gray ||
        count != mask.pixels.size() || count > budget ||
        !mask.icc_profile.empty() || mask.srgb_intent)
        throw WatermarkError("Expected a bounded, untagged gray8 coverage mask");
}
std::uint8_t quantize(double value) {
    return static_cast<std::uint8_t>(std::clamp(std::floor(value + 0.5), 0.0, 255.0));
}
/** Source-mask samples contributing along one axis. No contribution table is
 * allocated. Coverage outside the positioned mask is represented by visible. */
struct AxisSamples {
    std::uint32_t first = 0, end = 0;
    double begin = 0, finish = 0, center = 0, visible = 0;
    bool area = false;

    double weight(std::uint32_t sample) const {
        if (area)
            return visible * (std::min(finish, double(sample)+1)-std::max(begin, double(sample)))
                / (finish-begin);
        if (end-first == 1) return visible;
        return visible * (sample == first ? 1-(center-first) : center-first);
    }
};
AxisSamples samples(std::uint32_t pixel, std::uint32_t canvas, std::uint32_t level,
                    std::uint32_t position, std::uint32_t extent, std::uint32_t source) {
    // Clip in integer canvas/level coordinates BEFORE subtracting the mark origin.
    // All products fit uint64; this avoids loss of position at large origins.
    const auto left = static_cast<std::uint64_t>(pixel)*canvas;
    const auto right = (static_cast<std::uint64_t>(pixel)+1)*canvas;
    const auto start = static_cast<std::uint64_t>(position)*level;
    const auto stop = (static_cast<std::uint64_t>(position)+extent)*level;
    const auto lo = std::max(left,start), hi = std::min(right,stop);
    if (lo >= hi) return {};
    const double denominator = static_cast<double>(extent)*level;
    const double begin = std::clamp(double(lo-start)/denominator*source,0.0,double(source));
    const double finish = std::clamp(double(hi-start)/denominator*source,0.0,double(source));
    const double visible = double(hi-lo)/canvas;
    const bool area = static_cast<std::uint64_t>(canvas)*source >=
                      static_cast<std::uint64_t>(extent)*level;
    if (area) {
        return {static_cast<std::uint32_t>(std::floor(begin)),
                static_cast<std::uint32_t>(std::ceil(finish)),begin,finish,0,visible,true};
    }
    // Linear enlargement uses pixel centers. At a boundary use the center of
    // the visible footprint and clamp to the edge sample; visibility still
    // accounts for the transparent portion outside the mark.
    const double center = std::clamp((begin+finish)/2-0.5,0.0,double(source-1));
    const auto first = static_cast<std::uint32_t>(std::floor(center));
    const auto end = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        static_cast<std::uint64_t>(first)+2,source));
    return {first,end,begin,finish,center,visible,false};
}
double coverage_at(const PreparedWatermark& mark, const WatermarkGrid& grid,
                   std::uint32_t x, std::uint32_t y) {
    const auto sx = samples(x,mark.canvas_width,grid.width,mark.x,mark.width,mark.mask->width);
    const auto sy = samples(y,mark.canvas_height,grid.height,mark.y,mark.height,mark.mask->height);
    if (sx.first >= sx.end || sy.first >= sy.end) return 0;
    double value = 0;
    for (auto my=sy.first; my<sy.end; ++my) {
        const double wy = sy.weight(my);
        for (auto mx=sx.first; mx<sx.end; ++mx)
            value += mark.mask->pixels[static_cast<std::size_t>(my)*mark.mask->width+mx]
                * wy * sx.weight(mx);
    }
    return std::clamp(value/255.0,0.0,1.0);
}

}
PreparedWatermark prepare_watermark(RasterImage8 mask, std::uint32_t width,
    std::uint32_t height, double fraction, std::size_t budget) {
    return prepare_watermark(std::make_shared<const RasterImage8>(std::move(mask)),
                             width,height,fraction,budget);
}
PreparedWatermark prepare_watermark(std::shared_ptr<const RasterImage8> mask, std::uint32_t width,
    std::uint32_t height, double fraction, std::size_t budget) {
    if (!mask) throw WatermarkError("Missing watermark mask");
    validate_mask(*mask, budget);
    if (!width || !height || !std::isfinite(fraction) || fraction <= 0 || fraction > 1)
        throw WatermarkError("Invalid watermark canvas or relative width");
    const double scale = std::min(width * fraction / mask->width,
                                  static_cast<double>(height) / mask->height);
    const auto target_width = static_cast<std::uint32_t>(std::clamp(
        std::floor(mask->width * scale + 0.5), 1.0, static_cast<double>(width)));
    const auto target_height = static_cast<std::uint32_t>(std::clamp(
        std::floor(mask->height * scale + 0.5), 1.0, static_cast<double>(height)));
    return {std::move(mask), width, height, (width-target_width)/2, (height-target_height)/2,
            target_width,target_height};
}

RasterImage8 apply_watermark(RasterImage8 image, const PreparedWatermark& mark,
    std::span<const std::uint8_t> color, double opacity, std::uint32_t ox,
    std::uint32_t oy, std::size_t budget) {
    return apply_watermark(std::move(image),mark,color,opacity,
        WatermarkGrid{mark.canvas_width,mark.canvas_height,ox,oy},budget);
}

RasterImage8 apply_watermark(RasterImage8 image, const PreparedWatermark& mark,
    std::span<const std::uint8_t> color, double opacity, const WatermarkGrid& grid,
    std::size_t budget) {
    if (!mark.mask) throw WatermarkError("Missing watermark mask");
    validate_mask(*mark.mask, budget);
    const auto channels = image.channel_count();
    const auto count = static_cast<std::uint64_t>(image.width) * image.height;
    const bool alpha = channels == 2 || channels == 4;
    const auto colors = alpha ? channels-1 : channels;
    if (!channels || !image.width || !image.height || count > budget/channels ||
        count*channels != image.pixels.size() || color.size() != colors ||
        !std::isfinite(opacity) || opacity < 0 || opacity > 1 ||
        image.icc_profile.size() > budget-image.pixels.size() ||
        mark.mask->pixels.size() > budget-image.pixels.size()-image.icc_profile.size())
        throw WatermarkError("Invalid watermark raster, color, opacity or memory budget");
    // Widen before adding: caller-owned structs must not overflow validation.
    if (!mark.canvas_width || !mark.canvas_height || !mark.width || !mark.height ||
        static_cast<std::uint64_t>(mark.x)+mark.width > mark.canvas_width ||
        static_cast<std::uint64_t>(mark.y)+mark.height > mark.canvas_height ||
        !grid.width || !grid.height || grid.width > mark.canvas_width || grid.height > mark.canvas_height ||
        static_cast<std::uint64_t>(grid.origin_x)+image.width > grid.width ||
        static_cast<std::uint64_t>(grid.origin_y)+image.height > grid.height)
        throw WatermarkError("Watermark or crop lies outside the full-image canvas");
    if (opacity == 0) return image;
    for (std::uint32_t y=0; y<image.height; ++y) {
        for (std::uint32_t x=0; x<image.width; ++x) {
            const double source_alpha = coverage_at(mark,grid,x+grid.origin_x,y+grid.origin_y)*opacity;
            if (source_alpha == 0) continue;
            auto* pixel = image.pixels.data() + (static_cast<std::size_t>(y)*image.width+x)*channels;
            const double retained_alpha = (alpha ? pixel[colors]/255.0 : 1.0)*(1-source_alpha);
            const double output_alpha = source_alpha+retained_alpha;
            for (std::size_t c=0; c<colors; ++c)
                pixel[c] = quantize((color[c]*source_alpha+pixel[c]*retained_alpha)/output_alpha);
            if (alpha) {
                pixel[colors] = quantize(output_alpha*255);
                if (!pixel[colors]) std::fill_n(pixel,colors,0);
            }
        }
    }
    return image;
}
}
