#include "http/IiifInfo.h"
#include "http/ImageUri.h"

namespace sipi::http {
Json::Value make_info_document(const iiif::Resource& resource,
                               std::uint32_t width, std::uint32_t height,
                               std::string_view public_base_url) {
    Json::Value document;
    document["@context"] = "http://iiif.io/api/image/3/context.json";
    document["id"] = service_uri(resource, public_base_url);
    document["type"] = "ImageService3";
    document["profile"] = "level2";
    document["protocol"] = "http://iiif.io/api/image";
    document["width"] = Json::UInt(width);
    document["height"] = Json::UInt(height);
    return document;
}

} // namespace sipi::http
