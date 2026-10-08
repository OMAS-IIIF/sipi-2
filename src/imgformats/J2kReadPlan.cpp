#include "J2kReadPlan.h"
#include <algorithm>
#include <limits>

namespace sipi {
namespace {
using Wide = std::uint64_t;
Wide ceil_div(Wide n, Wide d) { return n / d + (n % d != 0); }
struct Axis { std::uint32_t level, first, length, source_first, source_length; PixelInterval window; };
Axis axis(std::uint32_t grid_origin, std::uint32_t base, std::uint32_t crop,
          std::uint32_t extent, Wide step) {
    const Wide grid_first = ceil_div(grid_origin, step);
    const Wide level = ceil_div(Wide{grid_origin}+base, step)-grid_first;
    const Wide left = Wide{crop}*level, right = (Wide{crop}+extent)*level;
    Wide first = left/base, end = ceil_div(right,base);
    // Grok treats windows whose every coordinate is <=1 as fractions. Include
    // an extra sample at the top-left if needed; the exact window discards it.
    if (first == 0 && end == 1 && level > 1) end = 2;
    const Wide source_first = first == 0 ? 0 : (grid_first+first)*step-grid_origin;
    const Wide source_end = std::min<Wide>(base,(grid_first+end)*step-grid_origin);
    return {static_cast<std::uint32_t>(level), static_cast<std::uint32_t>(first), static_cast<std::uint32_t>(end-first),
        static_cast<std::uint32_t>(source_first), static_cast<std::uint32_t>(source_end-source_first),
        {left%base, Wide{extent}*level, base}};
}
}
J2kReadPlan plan_j2k_read(const J2kMetadata& m, const iiif::ImageGeometry& g) {
    const auto& c = g.region;
    if (!m.width || !m.height || !m.resolution_count || m.resolution_count > 32 ||
        Wide{m.origin_x}+m.width > UINT32_MAX || Wide{m.origin_y}+m.height > UINT32_MAX ||
        !c.width || !c.height || !g.output_width || !g.output_height ||
        Wide{c.x}+c.width > m.width || Wide{c.y}+c.height > m.height)
        throw std::invalid_argument("Invalid JPEG 2000 read geometry");
    std::uint8_t reduction = 0;
    for (std::uint8_t r=1; r<m.resolution_count; ++r) {
        const Wide step = Wide{1}<<r;
        const Wide w = ceil_div(Wide{m.origin_x}+m.width,step)-ceil_div(m.origin_x,step);
        const Wide h = ceil_div(Wide{m.origin_y}+m.height,step)-ceil_div(m.origin_y,step);
        if (!w || !h || Wide{c.width}*w < Wide{g.output_width}*m.width ||
            Wide{c.height}*h < Wide{g.output_height}*m.height) break;
        reduction = r;
    }
    const Wide step = Wide{1}<<reduction;
    const auto x = axis(m.origin_x,m.width,c.x,c.width,step);
    const auto y = axis(m.origin_y,m.height,c.y,c.height,step);
    return {reduction, x.level, y.level, {x.source_first,y.source_first,x.source_length,y.source_length},
        {x.first,y.first,x.length,y.length}, {x.window,y.window}};
}
} // namespace sipi
