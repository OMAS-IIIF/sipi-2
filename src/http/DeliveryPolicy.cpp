#include "DeliveryPolicy.h"
#include "image/OutputLimits.h"
#include "image/ImageTransform.h"
#include "imgformats/TiffReader.h"
#include "imgformats/J2kReadPlan.h"
#include "imgformats/JpegWriter.h"
#include <algorithm>
#include <initializer_list>
#include <type_traits>

namespace sipi::http {
namespace {
constexpr std::uint64_t working_limit = 256 * 1024 * 1024;

/** Bounded multiplication avoids overflow even for hostile uint32 dimensions. */
std::uint64_t bytes(std::initializer_list<std::uint64_t> factors) {
    std::uint64_t result = 1;
    for (auto factor : factors) {
        if (!factor || factor > working_limit / result)
            throw DeliveryLimitError("Decoded raster or block exceeds working budget");
        result *= factor;
    }
    return result;
}
void fits(std::initializer_list<std::uint64_t> parts) {
    auto available = working_limit;
    for (auto part : parts) {
        if (part > available) throw DeliveryLimitError("Image processing exceeds working budget");
        available -= part;
    }
}
}
iiif::SizePolicy bounded_output_policy(iiif::SizePolicy policy) {
    policy.max_width = policy.max_width.value_or(8192);
    policy.max_height = policy.max_height.value_or(*policy.max_width);
    policy.max_area = policy.max_area.value_or(OutputLimits::area_ceiling);
    if (!*policy.max_width || !*policy.max_height || !*policy.max_area ||
        *policy.max_width > OutputLimits::axis_ceiling ||
        *policy.max_height > OutputLimits::axis_ceiling ||
        *policy.max_area > OutputLimits::area_ceiling)
        throw std::invalid_argument("Output policy exceeds renderer limits");
    return policy;
}

TiffReadPlan supported_tiff_plan(const TiffMetadata& metadata, const iiif::ImageGeometry& geometry) {
    TiffMetadata candidates;
    candidates.image = metadata.image;
    for (const auto& level : metadata.levels)
        if (supports_tiff_region(level)) candidates.levels.push_back(level);
    return plan_tiff_read(candidates, geometry);
}

void validate_raster_delivery_budget(std::uint64_t raster, std::size_t profile,
                                    const iiif::ImageGeometry& geometry, const iiif::Rotation& rotation) {
    const auto output = bytes({geometry.output_width,geometry.output_height,4});
    // Conservative even when resize is an identity; no second decoder raster.
    fits({raster,output,profile});
    const auto rotated = rotated_size(geometry.output_width,geometry.output_height,rotation.degrees);
    if (rotated.width > jpeg_maximum_dimension() || rotated.height > jpeg_maximum_dimension() ||
        std::uint64_t{rotated.width} * rotated.height > OutputLimits::area_ceiling)
        throw DeliveryLimitError("Rotated canvas exceeds encoder/output working limit");
    const auto transformed = bytes({rotated.width,rotated.height,4});
    fits({output,transformed,profile});
    // Color conversion: RGBA plus gray-alpha, source/target profiles and
    // subsequent two-row dithering scratch. Reserve 1 MiB for our tiny target ICC.
    fits({transformed,transformed/2,profile,1024*1024,
          (std::uint64_t{rotated.width}+2)*2*sizeof(double)});
}

void validate_delivery_budget(const cache::ImageMetadata& metadata,
                              const iiif::ImageGeometry& geometry, const iiif::Rotation& rotation) {
    std::visit([&](const auto& source) {
        using T = std::decay_t<decltype(source)>;
        std::uint64_t raster = 0;
        const auto profile = source.icc_profile.size();
        if (profile > jpeg_maximum_icc_bytes())
            throw DeliveryLimitError("Source ICC exceeds common JPEG/PNG output capacity");
        if constexpr (std::is_same_v<T, TiffMetadata>) {
            const auto plan = supported_tiff_plan(source, geometry);
            const auto& d = plan.directory;
            const auto& r = plan.read_region;
            raster = bytes({r.width,r.height,d.samples_per_pixel});
            const auto block = std::visit([&](const auto& layout) {
                using L = std::decay_t<decltype(layout)>;
                if constexpr (std::is_same_v<L, TiffTiles>)
                    return bytes({layout.width,layout.height,d.samples_per_pixel});
                else {
                    if (!layout.rows_per_strip) throw DeliveryLimitError("Invalid TIFF strip height");
                    const auto first_y = r.y / layout.rows_per_strip * layout.rows_per_strip;
                    return bytes({d.width,std::min(layout.rows_per_strip,d.height-first_y),d.samples_per_pixel});
                }
            },d.layout);
            fits({raster,block,profile});
        } else if constexpr (std::is_same_v<T, J2kMetadata>) {
            const auto plan = plan_j2k_read(source,geometry);
            const auto& r = plan.source_region;
            const std::uint64_t channels = source.components.size();
            raster = bytes({plan.reduced_region.width,plan.reduced_region.height,channels});
            const std::uint64_t step = std::uint64_t{1} << plan.reduction;
            const auto span = [](std::uint64_t first, std::uint64_t extent,
                                 std::uint64_t origin, std::uint64_t tile) {
                if (!tile || first < origin) throw DeliveryLimitError("Invalid JPEG 2000 tile grid");
                return (first+extent-1-origin)/tile - (first-origin)/tile + 1;
            };
            const auto tiles = bytes({
                span(std::uint64_t{source.origin_x}+r.x,r.width,source.tile_origin_x,source.tile_width),
                span(std::uint64_t{source.origin_y}+r.y,r.height,source.tile_origin_y,source.tile_height),
                (std::uint64_t{source.tile_width}+step-1)/step,
                (std::uint64_t{source.tile_height}+step-1)/step, channels, sizeof(std::int32_t)});
            fits({raster,tiles,profile});
        } else if constexpr (std::is_same_v<T, JpegMetadata>) {
            raster = bytes({source.width,source.height,source.color==JpegColor::gray ? 1u : 3u});
            fits({raster,source.coefficient_bytes,profile});
        } else {
            const bool gray = source.color==PngColor::gray || source.color==PngColor::gray_alpha;
            const unsigned channels = (gray ? 1u : 3u) + (source.transparency ? 1u : 0u);
            raster = bytes({source.width,source.height,channels});
            fits({raster,profile});
        }
        validate_raster_delivery_budget(raster,profile,geometry,rotation);
    }, metadata);
}
} // namespace sipi::http
