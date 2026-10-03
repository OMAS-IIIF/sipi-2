#include "http/IiifInfo.h"
#include <string>

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

Json::Value make_info_document(const iiif::Resource& resource,
                               std::uint32_t width, std::uint32_t height,
                               std::string_view public_base_url) {
    std::string id{public_base_url};
    for (const auto& prefix : resource.prefix) id += '/' + encode_segment(prefix);
    id += '/' + encode_segment(resource.identifier);

    Json::Value document;
    document["@context"] = "http://iiif.io/api/image/3/context.json";
    document["id"] = std::move(id);
    document["type"] = "ImageService3";
    document["protocol"] = "http://iiif.io/api/image";
    document["width"] = Json::UInt(width);
    document["height"] = Json::UInt(height);
    return document;
}

} // namespace sipi::http
