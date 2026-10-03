#include "iiifparser/IIIFParser.h"

#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <variant>

namespace {
using namespace sipi::iiif;
int checks = 0;
int failures = 0;

void check(bool condition, std::string_view description) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << description << '\n';
    }
}

std::string image_path(std::string_view region = "full", std::string_view size = "max",
                       std::string_view rotation = "0", std::string_view quality = "default.jpg") {
    return "/id/" + std::string(region) + '/' + std::string(size) + '/' +
           std::string(rotation) + '/' + std::string(quality);
}

void rejects(std::string_view path, Component component) {
    const auto result = parse_path(path);
    const auto* error = std::get_if<ParseError>(&result);
    check(error && error->component == component && !error->message.empty(), path);
}

/** Check both successful parsing and the actual values, not just variant indices. */
void valid_requests() {
    const auto result = parse_path("/myproj/subpart/imageid/full/max/0/default.jpg");
    const auto* image = std::get_if<ImageRequest>(&result);
    check(image != nullptr, "image request");
    if (image) {
        check(image->resource.prefix == std::vector<std::string>{"myproj", "subpart"}, "prefix boundaries");
        check(image->resource.identifier == "imageid", "identifier");
        check(std::holds_alternative<FullRegion>(image->region), "full region");
        check(std::holds_alternative<MaxSize>(image->size.value) && !image->size.allow_upscaling, "max size");
        check(!image->rotation.mirrored && image->rotation.degrees == 0, "zero rotation");
        check(image->quality == Quality::default_quality && image->format == Format::jpg, "default jpg");
    }
    // The temporary input is destroyed before the result is examined.
    const auto owned = parse_path(std::string("/project%2Fpart/a%252Fb+c%2Fd/info.json"));
    const auto* info = std::get_if<InfoRequest>(&owned);
    check(info && info->resource.prefix == std::vector<std::string>{"project/part"} &&
          info->resource.identifier == "a%2Fb+c/d", "owning strings, split before decoding, decode once, retain plus");
    const auto root_info = parse_path("/id/info.json");
    check(std::holds_alternative<InfoRequest>(root_info) &&
          std::get<InfoRequest>(root_info).resource.prefix.empty(), "info without prefix");
    const auto escaped = parse_path("/a%3Fb%23c%20d/%66ull/max/%210/default%2Ejpg");
    check(std::holds_alternative<ImageRequest>(escaped) &&
          std::get<ImageRequest>(escaped).resource.identifier == "a?b#c d" &&
          std::get<ImageRequest>(escaped).rotation.mirrored, "encoded delimiters and parameter decoding");

    const auto square = parse_path(image_path("square"));
    check(std::holds_alternative<ImageRequest>(square) &&
          std::holds_alternative<SquareRegion>(std::get<ImageRequest>(square).region), "square region");
    const auto pixels = parse_path(image_path("0,12,4294967295,34"));
    if (const auto* request = std::get_if<ImageRequest>(&pixels)) {
        const auto* region = std::get_if<PixelRegion>(&request->region);
        check(region && region->x == 0 && region->y == 12 && region->width ==
              std::numeric_limits<std::uint32_t>::max() && region->height == 34, "pixel region and uint32 boundary");
    } else { check(false, "pixel region accepted"); }
    const auto percentages = parse_path(image_path("pct:.5,01.00,120,3.25"));
    if (const auto* request = std::get_if<ImageRequest>(&percentages)) {
        const auto* region = std::get_if<PercentRegion>(&request->region);
        check(region && region->x == .5 && region->y == 1 && region->width == 120 && region->height == 3.25,
              "percentage region; clipping deferred");
    } else { check(false, "percentage region accepted"); }

    for (const bool upscale : {false, true}) {
        for (const std::string_view token : {"max", "12,", ",34", "12,34", "!12,34", "pct:12.5"}) {
            const auto parsed = parse_path(image_path("full", (upscale ? "^" : "") + std::string(token)));
            const auto* request = std::get_if<ImageRequest>(&parsed);
            check(request && request->size.allow_upscaling == upscale, token);
            if (!request) { continue; }
            const auto& value = request->size.value;
            if (token == "max") { check(std::holds_alternative<MaxSize>(value), token); }
            if (token == "12,") { const auto* p = std::get_if<WidthSize>(&value); check(p && p->width == 12, token); }
            if (token == ",34") { const auto* p = std::get_if<HeightSize>(&value); check(p && p->height == 34, token); }
            if (token == "12,34") { const auto* p = std::get_if<ExactSize>(&value); check(p && p->width == 12 && p->height == 34, token); }
            if (token == "!12,34") { const auto* p = std::get_if<FitSize>(&value); check(p && p->width == 12 && p->height == 34, token); }
            if (token == "pct:12.5") { const auto* p = std::get_if<PercentSize>(&value); check(p && p->percent == 12.5, token); }
        }
    }
    check(std::holds_alternative<ImageRequest>(parse_path(image_path("full", "^pct:150"))), "explicit percentage upscaling");
    for (const std::string_view token : {"0", "360", "!360", ".5", "!01.50", "1."}) {
        const auto parsed = parse_path(image_path("full", "max", token));
        const auto* request = std::get_if<ImageRequest>(&parsed);
        check(request && request->rotation.mirrored == token.starts_with('!'), token);
        if (token == "!01.50") { check(request && request->rotation.degrees == 1.5, "fractional rotation"); }
    }
    const std::pair<std::string_view, Quality> qualities[] = {
        {"default", Quality::default_quality}, {"color", Quality::color},
        {"gray", Quality::gray}, {"bitonal", Quality::bitonal}};
    const std::pair<std::string_view, Format> formats[] = {
        {"jpg", Format::jpg}, {"tif", Format::tif}, {"png", Format::png},
        {"gif", Format::gif}, {"jp2", Format::jp2}, {"pdf", Format::pdf}, {"webp", Format::webp}};
    for (const auto& [quality_name, quality] : qualities) {
        for (const auto& [format_name, format] : formats) {
            const auto parsed = parse_path(image_path("full", "max", "0", std::string(quality_name) + '.' + std::string(format_name)));
            const auto* request = std::get_if<ImageRequest>(&parsed);
            check(request && request->quality == quality && request->format == format, "quality/format mapping");
        }
    }
}

void invalid_requests() {
    for (const auto path : {"/./id/info.json", "/a/%2e%2E/id/info.json", "/%2E/info.json"}) {
        rejects(path, Component::path);
    }
    for (const std::string_view path : {"", "/", "id/info.json", "/id", "/info.json", "/id/full/max/0",
         "/id/info.json/", "/p//id/info.json", "/id/info.json?x=1", "/id/info.json#fragment",
         "/%/info.json", "/%2/info.json", "/%gg/info.json", "/%00/info.json", "/%0a/info.json",
         "/%7f/info.json", "/a\nb/info.json"}) {
        rejects(path, Component::path);
    }
    for (const std::string_view region : {"FULL", "fullx", "1,2,3", "1,2,3,4,5", "-1,0,1,1", "+1,0,1,1",
         "0,0,0,1", "0,0,1,0", "0,0,1.5,2", "0,0,1x,2", "4294967296,0,1,1", "0,0,4294967297,1",
         "pct:0,0,0,1", "pct:0,0,-1,1", "pct:nan,0,1,1", "pct:0,0,inf,1", "pct:0,0,1e2,1",
         "pct:0,0,.,1", "pct:0,0,1..5,1", "pct:0,0,1,1x", "pct:0, 0,1,1"}) {
        rejects(image_path(region), Component::region);
    }
    for (const std::string_view size : {"full", "red:2", "^", "!max", "^!max", "^^max", "!^12,34", ",",
         "0,", ",0", "12,0", "0,34", "!12,", "!,34", "12", "12,34,56", "12.5,", "12x,",
         "-12,", "+12,", "4294967296,", "4294967297,", "pct:0", "pct:100.1", "pct:nan",
         "pct:1e2", "pct:inf", "pct:-1", "pct:", "pct:.", "^pct:0", "maxjunk"}) {
        rejects(image_path("full", size), Component::size);
    }
    for (const std::string_view rotation : {"!", "!!0", "-1", "+1", "360.1", "361", "nan", "inf", "1e2", "0x", ".", "1..2", " 0"}) {
        rejects(image_path("full", "max", rotation), Component::rotation);
    }
    for (const std::string_view quality : {".jpg", "default.", "default", "DEFAULT.jpg", "default.JPG",
         "default.jpeg", "default.tiff", "default.jpg.extra", "unknown.jpg", "default.avif"}) {
        rejects(image_path("full", "max", "0", quality), Component::quality_format);
    }
    // Extremely large/small decimals must not silently become infinity or zero.
    rejects(image_path("full", "max", std::string(400, '9')), Component::rotation);
    rejects(image_path("full", "^pct:0." + std::string(400, '0') + "1"), Component::size);
    rejects(image_path("full", "max", ""), Component::path);
    rejects(image_path("", "max"), Component::path);
}
} // namespace

int main() {
    valid_requests();
    invalid_requests();
    std::cout << checks << " IIIF parser checks; " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
