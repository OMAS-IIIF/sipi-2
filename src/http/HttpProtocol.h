#pragma once
#include "iiifparser/IIIFParser.h"
#include "storage/ByteSource.h"
#include <optional>
#include <map>
#include <string>
#include <vector>
#include <string_view>

namespace sipi::http {
using QueryParameters = std::map<std::string, std::vector<std::string>>;
enum class QueryError { malformed, limit };
/** Parse the raw query without '?', before policy/cache lookup. Percent-decode once,
 * '+' means space; retain duplicate values in wire order. Empty values are valid,
 * empty names/components, controls and raw fragments are rejected. Decoded strings
 * are bytes, without Unicode normalization. Limits: 32 KiB raw, 128 pairs,
 * 256-byte names and 16 KiB values. Errors never contain credentials. */
[[nodiscard]] std::variant<QueryParameters, QueryError> parse_query_parameters(std::string_view raw);

/** Parsed IIIF operation; base URIs reuse info authorization/admission before 303.
 * Image-shaped suffixes take precedence over arbitrary-prefix base identities.
 * Invalid image syntax must not turn into a redirect to a different resource.
 */
struct RoutedIiifRequest {
    iiif::ParseResult query;
    bool redirect_to_info = false;
};
[[nodiscard]] RoutedIiifRequest route_iiif_path(std::string_view path);

enum class InfoMediaType { json, json_ld };
/** Select JSON/JSON-LD using Accept specificity, parameters and q-values.
 * Absent Accept prefers JSON-LD; explicit exclusions beat wildcards. Unknown
 * representation parameters do not match; extensions after q are ignored.
 * Invalid ranges are ignored; no acceptable range returns nullopt (HTTP 406).
 * No I/O, retained views, mutation or shared state.
 */
[[nodiscard]] std::optional<InfoMediaType> negotiate_info_type(
    std::optional<std::string_view> accept);

/** CORS method/header preflight supports GET/HEAD and explicit
 * Authorization, Accept, Content-Type headers. Origin/credentials policy is applied
 * separately to responses. Never grants image authorization. */
[[nodiscard]] bool allowed_cors_preflight(std::string_view method, std::string_view headers);

/** Client-facing description of a storage read failure. Diagnostics and
 * source paths stay internal; retry_after is in seconds when present. */
struct StorageFailure {
    int status;
    const char* code;    // Static, stable JSON error name.
    const char* message; // Static text without paths or credentials.
    std::optional<unsigned> retry_after;
};
/** not_found -> 404. changed -> 503 with Retry-After: the source was replaced
 * while it was being read, and a new request opens the new version. Any other
 * storage failure -> 500. retry_after is set exactly for 503. No I/O or shared state. */
[[nodiscard]] StorageFailure storage_failure(ByteSourceErrorCode code) noexcept;

inline constexpr std::string_view jsonld_content_type =
    "application/ld+json;profile=\"http://iiif.io/api/image/3/context.json\"";
inline constexpr std::string_view level2_link =
    "<http://iiif.io/api/image/3/level2.json>;rel=\"profile\"";
} // namespace sipi::http
