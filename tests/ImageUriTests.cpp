#include "http/ImageUri.h"
#include <iostream>
#include <stdexcept>

namespace {
/** Resolve real parser input, compare a known canonical suffix, then replay it.
 * Replaying must preserve the concrete crop/size and produce the same URI.
 */
void check(std::string_view suffix, std::string_view expected, bool gray = false,
           sipi::iiif::SizePolicy policy = {}) {
    using namespace sipi;
    const std::string base = "https://images.example/mount/project%2Fpart/id%25";
    const auto parsed = iiif::parse_path("/project%2fpart/id%25/" + std::string(suffix));
    const auto& request = std::get<iiif::ImageRequest>(parsed);
    const auto geometry = std::get<iiif::ImageGeometry>(iiif::resolve_geometry(
        request.region, request.size, 300, 200, policy));
    const auto canonical = http::canonical_image_uri(request, geometry, 300, 200,
                                                     policy, gray, "https://images.example/mount");
    if (canonical != base + '/' + std::string(expected))
        throw std::runtime_error(std::string(suffix) + " produced " + canonical);
    const auto replay = std::get<iiif::ImageRequest>(iiif::parse_path(canonical.substr(
        std::string("https://images.example/mount").size())));
    const auto next = std::get<iiif::ImageGeometry>(iiif::resolve_geometry(
        replay.region, replay.size, 300, 200, policy));
    if (next.region.x != geometry.region.x || next.region.y != geometry.region.y ||
        next.region.width != geometry.region.width || next.region.height != geometry.region.height ||
        next.output_width != geometry.output_width || next.output_height != geometry.output_height ||
        http::canonical_image_uri(replay, next, 300, 200, policy, gray,
                                  "https://images.example/mount") != canonical)
        throw std::runtime_error("Canonical replay changed geometry or identity");
}
}
int main() {
    try {
        check("full/max/0/default.jpg", "full/max/0/default.jpg");
        check("0,0,999,999/300,200/360/color.png", "full/max/0/default.png");
        check("pct:0,0,100,100/pct:50/!090.000/color.jpg", "full/150,100/!90/default.jpg");
        check("square/100,/0/gray.png", "50,0,200,200/100,100/0/gray.png");
        check("250,150,100,100/max/0/bitonal.png", "250,150,50,50/max/0/bitonal.png");
        check("pct:10,10,50,50/!100,100/12.3400/default.jpg", "30,20,150,100/100,67/12.34/default.jpg");
        check("full/,100/0/default.png", "full/150,100/0/default.png");
        check("full/150,80/0/default.png", "full/150,80/0/default.png");
        check("full/^300,200/0/default.png", "full/max/0/default.png");
        check("full/max/0/gray.png", "full/max/0/default.png", true);
        check("full/max/0/bitonal.png", "full/max/0/bitonal.png", true);
        check("full/max/0.0000001/default.jpg", "full/max/0.0000001/default.jpg");
        check("full/max/359.99999999999994/default.jpg", "full/max/359.99999999999994/default.jpg");
        sipi::iiif::SizePolicy limits{false, 150, 100, 15000};
        check("full/150,100/0/default.jpg", "full/max/0/default.jpg", false, limits);
        check("0,0,100,100/100,100/0/default.jpg", "0,0,100,100/max/0/default.jpg", false, limits);
        check("full/75,50/0/default.jpg", "full/75,50/0/default.jpg", false, limits);
        limits.allow_upscaling = true;
        check("0,0,50,50/^100,100/0/default.jpg", "0,0,50,50/^max/0/default.jpg", false, limits);
        check("0,0,50,50/^75,75/0/default.jpg", "0,0,50,50/^75,75/0/default.jpg", false, limits);
        if (sipi::http::service_uri({{"a/b", "ü"}, "a+%?#<>"}, "https://host") !=
            "https://host/a%2Fb/%C3%BC/a%2B%25%3F%23%3C%3E")
            throw std::runtime_error("Public segment encoding changed");
        std::cout << "Canonical forms and geometry replay passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
