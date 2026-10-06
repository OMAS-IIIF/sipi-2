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
 * Caller must admit the source under the shared delivery policy before publishing
 * this level2 document. The HTTP stack supplies negotiation, redirects and CORS;
 * source-specific extra qualities/features and limits are added by the worker.
 */
[[nodiscard]] Json::Value make_info_document(const iiif::Resource& resource,
                                            std::uint32_t width, std::uint32_t height,
                                            std::string_view public_base_url);

} // namespace sipi::http
