#ifndef SIPI_PREFLIGHT_LUA_PREFLIGHT_H
#define SIPI_PREFLIGHT_LUA_PREFLIGHT_H

#include "iiifparser/IIIFParser.h"
#include "image/ImageSource.h"

#include <filesystem>
#include <optional>
#include <map>
#include <stdexcept>
#include <string>
#include <variant>

namespace sipi::preflight {

/** Owning input to policy evaluation. Resource names are already decoded by the
 * IIIF parser, but are not filesystem paths. Missing headers remain absent rather
 * than becoming empty strings. Credentials must not be logged. */
struct Request {
    iiif::Resource resource;
    std::optional<std::string> authorization;
    std::optional<std::string> cookie;
    // HTTP adapters can supply parsed cookies when the raw header is unavailable.
    std::map<std::string, std::string> cookies{};
};

/** Access granted, with an optional local path or S3 key override. No source means that
 * the caller must resolve the resource using its configured mapping. Paths are
 * copied, not opened, normalized, or checked against repository policy here. */
struct Allow {
    std::optional<SourceOverride> source;
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
 * and invokes preflight(request). No Lua state or request data survives a call.
 * Concurrent const evaluations are independent; destruction must not race a call.
 *
 * Each state is limited to 16 MiB of Lua allocations and one million VM instructions.
 * These are not wall-clock limits for native calls. No host network facilities yet.
 * It is not a sandbox and must not run untrusted code or on an HTTP I/O thread.
 * Reconstruct the object to adopt file changes. See docs/preflight.md for the
 * strict table contract, available libraries, and deferred HTTP integration. */
class LuaPreflight {
public:
    /** Read a text script, throwing PreflightError(script_file) on an I/O error.
     * Syntax and runtime validation happen on evaluate, not during construction. */
    explicit LuaPreflight(const std::filesystem::path& script_path);

    /** Compile and run top-level initialization in a fresh state, then check that
     * preflight is a function without calling it. Throws PreflightError on failure.
     * This detects startup errors, not request-dependent errors or time spent inside native calls. */
    void validate() const;

    /** Return an owning decision or throw PreflightError on Lua/contract failure.
     * std::bad_alloc from C++ allocations may also propagate. No image/file lookup
     * occurs here, and the caller must treat every failure as unsuccessful. */
    [[nodiscard]] Decision evaluate(const Request& request) const;

private:
    std::string script_;
    std::string chunk_name_;
};

} // namespace sipi::preflight

#endif
