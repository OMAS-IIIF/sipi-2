#pragma once

#include "iiifparser/IIIFParser.h"
#include <json/json.h>
#include <cstdint>
#include <string_view>

namespace sipi::http {

/** Build information from a public resource identity and display dimensions.
 * Format-neutral: callers supply oriented dimensions of the authorized source.
 * public_base_url is validated startup configuration without a trailing slash;
 * resource contains once-decoded path segments. No filesystem paths or request
 * headers enter the document. The returned JSON owns all strings.
 *
 * Development boundary agreed with the owner: profile is absent until image
 * output can satisfy an IIIF compliance level. Do not infer advertised output
 * capabilities from the parser, TIFF storage layout, or upscaling setting.
 */
[[nodiscard]] Json::Value make_info_document(const iiif::Resource& resource,
                                            std::uint32_t width, std::uint32_t height,
                                            std::string_view public_base_url);

} // namespace sipi::http
