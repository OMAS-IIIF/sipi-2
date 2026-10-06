#include "ImageUri.h"
#include <charconv>
#include <array>
#include <stdexcept>

namespace sipi::http {
namespace {

/** Encode each decoded segment separately; slash and percent are data here.
 * UTF-8 is escaped byte-wise. Only RFC 3986 unreserved ASCII remains literal.
 */
std::string encode_segment(std::string_view segment) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    for (const unsigned char byte : segment) {
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' || byte == '_' || byte == '~') {
            encoded += static_cast<char>(byte);
        } else {
            encoded += '%';
            encoded += hex[byte >> 4];
            encoded += hex[byte & 15];
        }
    }
    return encoded;
}

} // namespace


std::string service_uri(const iiif::Resource& resource, std::string_view public_base_url) {
    std::string uri{public_base_url};
    for (const auto& prefix : resource.prefix) uri += '/' + encode_segment(prefix);
    return uri + '/' + encode_segment(resource.identifier);
}

std::string canonical_image_uri(const iiif::ImageRequest& request,
    const iiif::ImageGeometry& geometry, std::uint32_t width, std::uint32_t height,
    const iiif::SizePolicy& policy, bool native_gray, std::string_view public_base_url) {
    const auto& crop = geometry.region;
    std::string uri = service_uri(request.resource, public_base_url) + '/';
    if (crop.x == 0 && crop.y == 0 && crop.width == width && crop.height == height) {
        uri += "full";
    } else {
        uri += std::to_string(crop.x) + ',' + std::to_string(crop.y) + ',' +
               std::to_string(crop.width) + ',' + std::to_string(crop.height);
    }
    uri += '/';
    // Re-resolve max on the concrete crop, not on the original full image.
    const auto matches_max = [&](bool upscale) {
        const auto result = iiif::resolve_geometry(crop, {upscale, iiif::MaxSize{}},
                                                   width, height, policy);
        const auto* maximum = std::get_if<iiif::ImageGeometry>(&result);
        return maximum && maximum->output_width == geometry.output_width &&
                          maximum->output_height == geometry.output_height;
    };
    const bool enlarged = geometry.output_width > crop.width || geometry.output_height > crop.height;
    if (!enlarged && matches_max(false)) {
        uri += "max";
    } else if (enlarged && matches_max(true)) {
        uri += "^max";
    } else {
        if (enlarged) uri += '^';
        uri += std::to_string(geometry.output_width) + ',' + std::to_string(geometry.output_height);
    }
    uri += '/';
    if (request.rotation.mirrored) uri += '!';
    // Shortest fixed decimal round-trips the parsed double without scientific
    // notation or locale dependence, even for subnormal angles. 360 equals 0.
    const double degrees = request.rotation.degrees == 360 ? 0 : request.rotation.degrees;
    std::array<char, 1100> angle{};
    const auto converted = std::to_chars(angle.data(), angle.data() + angle.size(),
                                         degrees == 0 ? 0.0 : degrees, std::chars_format::fixed);
    if (converted.ec != std::errc{}) throw std::logic_error("Cannot serialize rotation");
    uri.append(angle.data(), converted.ptr);
    uri += '/';
    switch (request.quality) {
    case iiif::Quality::default_quality:
    case iiif::Quality::color: uri += "default"; break;
    case iiif::Quality::gray: uri += native_gray ? "default" : "gray"; break;
    case iiif::Quality::bitonal: uri += "bitonal"; break;
    }
    switch (request.format) {
    case iiif::Format::jpg: uri += ".jpg"; break;
    case iiif::Format::png: uri += ".png"; break;
    case iiif::Format::tif: uri += ".tif"; break;
    case iiif::Format::gif: uri += ".gif"; break;
    case iiif::Format::jp2: uri += ".jp2"; break;
    case iiif::Format::pdf: uri += ".pdf"; break;
    case iiif::Format::webp: uri += ".webp"; break;
    }
    return uri;
}
} // namespace sipi::http
