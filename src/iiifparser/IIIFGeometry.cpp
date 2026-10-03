#include "IIIFGeometry.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

namespace sipi::iiif {
namespace {

bool positive(double value) { return std::isfinite(value) && value > 0; }

/** Clip before arithmetic/conversion: even huge percentage extents stay bounded. */
std::optional<PixelRegion> resolve_region(const Region& region, std::uint32_t width,
                                         std::uint32_t height) {
    return std::visit([=](const auto& value) -> std::optional<PixelRegion> {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, FullRegion>) {
            return PixelRegion{0, 0, width, height};
        } else if constexpr (std::is_same_v<T, SquareRegion>) {
            const auto side = std::min(width, height);
            return PixelRegion{(width - side) / 2, (height - side) / 2, side, side};
        } else if constexpr (std::is_same_v<T, PixelRegion>) {
            if (value.x >= width || value.y >= height || value.width == 0 || value.height == 0) {
                return std::nullopt;
            }
            return PixelRegion{value.x, value.y, std::min(value.width, width - value.x),
                               std::min(value.height, height - value.y)};
        } else {
            if (!std::isfinite(value.x) || !std::isfinite(value.y) || value.x < 0 ||
                value.y < 0 || value.x >= 100 || value.y >= 100 ||
                !positive(value.width) || !positive(value.height)) {
                return std::nullopt;
            }
            const auto x = static_cast<std::uint32_t>(std::floor(value.x / 100 * width));
            const auto y = static_cast<std::uint32_t>(std::floor(value.y / 100 * height));
            const auto w = static_cast<std::uint32_t>(std::ceil(std::min(value.width, 100.0) / 100 * width));
            const auto h = static_cast<std::uint32_t>(std::ceil(std::min(value.height, 100.0) / 100 * height));
            if (w == 0 || h == 0) { return std::nullopt; }
            return PixelRegion{x, y, std::min(w, width - x), std::min(h, height - y)};
        }
    }, region);
}

/** Scale cap from explicit policy limits. Use division before multiplication. */
double scale_limit(std::uint32_t width, std::uint32_t height, const SizePolicy& policy) {
    double scale = std::numeric_limits<double>::infinity();
    if (policy.max_width) {
        scale = std::min(static_cast<double>(*policy.max_width) / width,
                         static_cast<double>(policy.max_height.value_or(*policy.max_width)) / height);
    }
    if (policy.max_area) {
        scale = std::min(scale, std::sqrt(static_cast<double>(*policy.max_area) / width / height));
    }
    return scale;
}

bool within_limits(std::uint32_t width, std::uint32_t height, const SizePolicy& policy) {
    return (!policy.max_width || (width <= *policy.max_width &&
            height <= policy.max_height.value_or(*policy.max_width))) &&
           (!policy.max_area || static_cast<std::uint64_t>(width) * height <= *policy.max_area);
}

} // namespace

GeometryResult resolve_geometry(const Region& region, const Size& size,
                                 std::uint32_t width, std::uint32_t height,
                                 const SizePolicy& policy) {
    if (width == 0 || height == 0) {
        return GeometryError{GeometryErrorCode::invalid_dimensions, "Image dimensions must be positive"};
    }
    if ((policy.max_height && !policy.max_width) ||
        (policy.max_width && *policy.max_width == 0) ||
        (policy.max_height && *policy.max_height == 0) ||
        (policy.max_area && *policy.max_area == 0)) {
        return GeometryError{GeometryErrorCode::invalid_policy, "Size limits must be positive; maxHeight requires maxWidth"};
    }
    const auto crop = resolve_region(region, width, height);
    if (!crop) {
        return GeometryError{GeometryErrorCode::invalid_region, "Region is empty, invalid, or outside the image"};
    }
    const double w = crop->width, h = crop->height;
    double output_w = 0, output_h = 0;
    // max and !w,h adapt to limits, whereas explicit dimensions must pass them.
    bool adaptive = false;
    bool unbounded = false;
    std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, MaxSize>) {
            adaptive = true;
            double scale = scale_limit(crop->width, crop->height, policy);
            if (!size.allow_upscaling) { scale = std::min(1.0, scale); }
            if (!std::isfinite(scale)) { unbounded = true; return; }
            output_w = w * scale;
            output_h = h * scale;
        } else if constexpr (std::is_same_v<T, WidthSize>) {
            output_w = value.width;
            output_h = h * (output_w / w);
        } else if constexpr (std::is_same_v<T, HeightSize>) {
            output_h = value.height;
            output_w = w * (output_h / h);
        } else if constexpr (std::is_same_v<T, ExactSize>) {
            output_w = value.width;
            output_h = value.height;
        } else if constexpr (std::is_same_v<T, FitSize>) {
            adaptive = true;
            double scale = std::min(value.width / w, value.height / h);
            if (!size.allow_upscaling) { scale = std::min(1.0, scale); }
            scale = std::min(scale, scale_limit(crop->width, crop->height, policy));
            output_w = w * scale;
            output_h = h * scale;
        } else {
            if (!positive(value.percent) || (!size.allow_upscaling && value.percent > 100)) { return; }
            output_w = w * (value.percent / 100);
            output_h = h * (value.percent / 100);
        }
    }, size.value);
    if (unbounded) {
        return GeometryError{GeometryErrorCode::unbounded_maximum, "^max needs a finite configured size limit"};
    }
    if (!positive(output_w) || !positive(output_h)) {
        return GeometryError{GeometryErrorCode::invalid_size, "Output dimensions must be finite and positive"};
    }
    // Round only after all scale choices; a zero-pixel result is not deliverable.
    const double rounded_w = std::round(output_w);
    const double rounded_h = std::round(output_h);
    constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
    if (rounded_w < 1 || rounded_h < 1 || rounded_w > maximum || rounded_h > maximum) {
        return GeometryError{GeometryErrorCode::invalid_size, "Output dimensions are outside the supported range [1, UINT32_MAX]"};
    }
    auto out_w = static_cast<std::uint32_t>(rounded_w);
    auto out_h = static_cast<std::uint32_t>(rounded_h);
    // Nearest rounding can cross a hard area boundary. Adaptive forms round down
    // in that case; explicit requests are rejected instead of silently resized.
    if (adaptive && !within_limits(out_w, out_h, policy)) {
        out_w = static_cast<std::uint32_t>(std::max(1.0, std::floor(output_w)));
        out_h = static_cast<std::uint32_t>(std::max(1.0, std::floor(output_h)));
    }
    if (!within_limits(out_w, out_h, policy)) {
        return GeometryError{GeometryErrorCode::size_limit_exceeded, "Requested size exceeds the configured limits"};
    }
    if (out_w > crop->width || out_h > crop->height) {
        if (!size.allow_upscaling) {
            return GeometryError{GeometryErrorCode::invalid_size, "Enlarging the region requires a leading ^"};
        }
        if (!policy.allow_upscaling) {
            return GeometryError{GeometryErrorCode::upscaling_disabled, "Upscaling is disabled by server policy"};
        }
    }
    return ImageGeometry{*crop, out_w, out_h};
}

} // namespace sipi::iiif
