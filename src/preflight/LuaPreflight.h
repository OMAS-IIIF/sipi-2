#ifndef SIPI_PREFLIGHT_LUA_PREFLIGHT_H
#define SIPI_PREFLIGHT_LUA_PREFLIGHT_H

#include "iiifparser/IIIFParser.h"
#include "image/ImageSource.h"
#include "preflight/ScriptConfig.h"

#include <filesystem>
#include <array>
#include <cstdint>
#include <optional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace sipi::preflight {
class SharedCache;

/** Transport-independent request context; all strings are owned by the request. */
using QueryParameters = std::map<std::string, std::vector<std::string>>;
enum class RequestKind { image, info, base };
enum class RequestMethod { get, head };

/** Owning input to policy evaluation. Resource names are already decoded by the
 * IIIF parser, but are not filesystem paths. Missing headers remain absent rather
 * than becoming empty strings. Credentials must not be logged. */
struct Request {
    iiif::Resource resource;
    std::optional<std::string> authorization;
    std::optional<std::string> cookie;
    // HTTP adapters can supply parsed cookies when the raw header is unavailable.
    std::map<std::string, std::string> cookies{};
    // Decoded exactly once by the HTTP adapter; duplicate values retain their order.
    QueryParameters query{};
    RequestKind kind = RequestKind::image;
    RequestMethod method = RequestMethod::get;
};

/** Owning single-color watermark policy, with no open storage/codec state.
 * Source follows the same local/S3 override contract as the main image.
 * Center on the full image; width_fraction in (0,1], opacity in [0,1].
 * Color is an sRGB8 triplet, not destination-encoded samples. */
struct Watermark {
    SourceOverride source;
    double width_fraction;
    double opacity;
    std::array<std::uint8_t,3> color;
};

/** Bound the entire public raster before any IIIF operation. Both axes are
 * positive; preserve aspect ratio and never enlarge. This is a detail restriction,
 * independent of per-response SizePolicy and optional upscaling. */
struct ResolutionLimit {
    std::uint32_t max_width;
    std::uint32_t max_height;
};

/** Access granted, with an optional local path or S3 key override. No source means that
 * the caller must resolve the resource using its configured mapping. Paths are
 * copied, not opened, normalized, or checked against repository policy here. */
struct Allow {
    std::optional<SourceOverride> source;
    std::optional<Watermark> watermark = std::nullopt;
    std::optional<ResolutionLimit> resolution = std::nullopt;
};

/** An explicit policy refusal, distinct from a broken or missing script. */
struct Deny {};
using Decision = std::variant<Allow, Deny>;

enum class ErrorCode { script_file, script_load, script_execution, contract, memory, instruction_limit };

/** Technical failure, never an implicit allow/deny decision. Diagnostics are for
 * operators and can contain script-supplied text; do not expose them to clients. */
class PreflightError : public std::runtime_error {
public:
    PreflightError(ErrorCode code, const std::string& message);
    [[nodiscard]] ErrorCode code() const noexcept { return code_; }

private:
    ErrorCode code_;
};

/** Snapshot of a trusted operator-written Lua script.
 * Construction reads the file; evaluate compiles and executes it in a fresh state
 * and invokes preflight(request). Lua states remain isolated; only explicitly cached JSON-compatible values survive calls.
 * Concurrent const evaluations share only the synchronized explicit cache; destruction must not race a call.
 *
 * Each state is limited to 16 MiB of Lua allocations and one million VM instructions.
 * Native HTTP has separate per-call and aggregate limits; see docs/lua-http.md.
 * It is not a sandbox and must not run untrusted code or on an HTTP I/O thread.
 * Reconstruct the object to adopt file changes. See docs/preflight.md for the
 * strict table contract and available libraries. */
class LuaPreflight {
public:
    /** Read a text script, throwing PreflightError(script_file) on an I/O error.
     * Snapshots explicit settings, throwing ScriptConfigError for invalid values.
     * Syntax and runtime validation happen on evaluate, not during construction. */
    explicit LuaPreflight(const std::filesystem::path& script_path, ScriptConfig config = {});

    /** Compile and run top-level initialization in a fresh state, then check that
     * preflight is a function without calling it. Throws PreflightError on failure.
     * HTTP requests are disabled during initialization; this does not validate remote services. */
    void validate() const;

    /** Return an owning decision or throw PreflightError on Lua/contract failure.
     * std::bad_alloc from C++ allocations may also propagate. No image/file lookup
     * occurs here, and the caller must treat every failure as unsuccessful. */
    [[nodiscard]] Decision evaluate(const Request& request) const;

private:
    std::string script_;
    std::string chunk_name_;
    ScriptConfig config_;
    std::shared_ptr<SharedCache> cache_; // Copies of the same immutable policy share its cache.
};

} // namespace sipi::preflight

#endif
