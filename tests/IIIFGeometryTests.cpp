#include "iiifparser/IIIFGeometry.h"

#include <iostream>
#include <limits>
#include <string>

namespace {
using namespace sipi::iiif;
int checks = 0, failures = 0;
void check(bool ok, const std::string& name) {
    ++checks;
    if (!ok) { ++failures; std::cerr << "FAIL: " << name << '\n'; }
}
GeometryResult resolve(std::string region, std::string size, const SizePolicy& policy = {},
                       std::uint32_t width = 1000, std::uint32_t height = 800) {
    const auto parsed = parse_path("/id/" + region + '/' + size + "/0/default.jpg");
    const auto* request = std::get_if<ImageRequest>(&parsed);
    if (!request) { throw std::runtime_error("Invalid syntax in geometry test fixture"); }
    return resolve_geometry(request->region, request->size, width, height, policy);
}
void dimensions(const GeometryResult& result, std::uint32_t w, std::uint32_t h, const std::string& name) {
    const auto* geometry = std::get_if<ImageGeometry>(&result);
    check(geometry && geometry->output_width == w && geometry->output_height == h, name);
}
void error(const GeometryResult& result, GeometryErrorCode code, const std::string& name) {
    const auto* failure = std::get_if<GeometryError>(&result);
    check(failure && failure->code == code && !failure->message.empty(), name);
}
} // namespace

int main() {
    using Code = GeometryErrorCode;
    dimensions(resolve("full", "max"), 1000, 800, "full max");
    const auto square = resolve("square", "max");
    dimensions(square, 800, 800, "square size");
    check(std::get<ImageGeometry>(square).region.x == 100 && std::get<ImageGeometry>(square).region.y == 0,
          "centered square");
    const auto odd = resolve("square", "max", {}, 5, 8);
    check(std::get<ImageGeometry>(odd).region.y == 1, "odd centered square floors origin");
    const auto clipped = resolve("900,700,4294967295,4294967295", "max");
    dimensions(clipped, 100, 100, "clip without unsigned overflow");
    check(std::get<ImageGeometry>(clipped).region.x == 900, "clip preserves origin");
    dimensions(resolve("pct:90,87.5,50,50", "max"), 100, 100, "percentage clipping");
    const auto fractional = resolve("pct:1,1,20,20", "max", {}, 101, 51);
    const auto& crop = std::get<ImageGeometry>(fractional).region;
    check(crop.x == 1 && crop.y == 0 && crop.width == 21 && crop.height == 11, "percentage rounding policy");
    for (const auto* region : {"1000,0,1,1", "0,800,1,1", "pct:100,0,1,1", "pct:0,101,1,1"}) {
        error(resolve(region, "max"), Code::invalid_region, region);
    }
    dimensions(resolve("100,100,500,400", "250,"), 250, 200, "size refers to region");
    dimensions(resolve("full", "500,"), 500, 400, "width only");
    dimensions(resolve("full", ",400"), 500, 400, "height only");
    dimensions(resolve("full", "500,100"), 500, 100, "exact distortion");
    dimensions(resolve("full", "pct:25"), 250, 200, "percentage");
    dimensions(resolve("full", "!500,300"), 375, 300, "confined aspect ratio");
    dimensions(resolve("full", "!2000,2000"), 1000, 800, "confined no accidental upscale");
    dimensions(resolve("full", "!2000,400"), 500, 400, "one oversized bound is valid");
    dimensions(resolve("full", "1,"), 1, 1, "nearest rounding");
    error(resolve("full", "pct:0.01"), Code::invalid_size, "zero-sized result");
    error(resolve("full", "1001,"), Code::invalid_size, "missing caret width");
    error(resolve("full", ",801"), Code::invalid_size, "missing caret height");
    error(resolve("full", "100,801"), Code::invalid_size, "missing caret exact");
    for (const auto* size : {"^2000,", "^,1600", "^2000,1600", "^!2000,1600", "^pct:200"}) {
        error(resolve("full", size), Code::upscaling_disabled, size);
        dimensions(resolve("full", size, {.allow_upscaling = true}), 2000, 1600, size);
    }
    for (const auto* size : {"^500,", "^,400", "^500,400", "^!500,400", "^pct:50"}) {
        dimensions(resolve("full", size), 500, 400, std::string("caret without enlargement ") + size);
    }
    const SizePolicy width_limit{.max_width = 400};
    dimensions(resolve("full", "max", width_limit), 400, 320, "maxWidth");
    dimensions(resolve("full", "max", width_limit, 800, 1000), 320, 400, "maxHeight defaults to maxWidth");
    const SizePolicy area_limit{.max_area = 200000};
    dimensions(resolve("full", "max", area_limit), 500, 400, "area cap");
    dimensions(resolve("full", "!900,700", area_limit), 500, 400, "confined respects area cap");
    error(resolve("full", "600,", area_limit), Code::size_limit_exceeded, "explicit size not silently clamped");
    error(resolve("full", "500,", width_limit), Code::size_limit_exceeded, "explicit width cap");
    const SizePolicy enlargement{.allow_upscaling = true, .max_width = 2000, .max_height = 1600};
    dimensions(resolve("full", "^max", enlargement), 2000, 1600, "bounded upscaled max");
    error(resolve("full", "^max", {.max_width = 2000}), Code::upscaling_disabled, "bounded max denied");
    dimensions(resolve("full", "^max", width_limit), 400, 320, "caret max downscale still valid");
    error(resolve("full", "^max", {.allow_upscaling = true}), Code::unbounded_maximum, "unbounded max has no finite answer");
    error(resolve("full", "max", {.max_height = 100}), Code::invalid_policy, "height requires width");
    error(resolve("full", "max", {.max_area = 0}), Code::invalid_policy, "zero limit");
    error(resolve("full", "max", {}, 0, 10), Code::invalid_dimensions, "zero source");
    constexpr auto max = std::numeric_limits<std::uint32_t>::max();
    dimensions(resolve("full", "max", {}, max, max), max, max, "uint32 maximum dimensions");
    error(resolve("full", "^pct:200", {.allow_upscaling = true}, max, max), Code::invalid_size, "overflow rejected");
    error(resolve_geometry(PercentRegion{0, 0, std::numeric_limits<double>::infinity(), 1},
                           Size{false, MaxSize{}}, 100, 100), Code::invalid_region, "manual nonfinite region");
    error(resolve_geometry(FullRegion{}, Size{true, PercentSize{std::numeric_limits<double>::quiet_NaN()}},
                           100, 100), Code::invalid_size, "manual NaN size");
    error(resolve_geometry(FullRegion{}, Size{false, ExactSize{0, 1}}, 100, 100), Code::invalid_size, "manual zero size");
    // Different small aspect ratios expose rounding/area-boundary mistakes.
    for (std::uint32_t w = 1; w <= 20; ++w) {
        for (std::uint32_t h = 1; h <= 20; ++h) {
            const auto result = resolve("full", "!7,9", {.max_area = 40}, w, h);
            if (const auto* geometry = std::get_if<ImageGeometry>(&result)) {
                check(geometry->output_width <= w && geometry->output_height <= h &&
                      geometry->output_width <= 7 && geometry->output_height <= 9 &&
                      geometry->output_width * geometry->output_height <= 40, "confined invariant");
            } else {
                check(std::get<GeometryError>(result).code == Code::invalid_size, "only subpixel failure expected");
            }
        }
    }
    std::cout << checks << " geometry checks; " << failures << " failures\n";
    return failures ? 1 : 0;
}
