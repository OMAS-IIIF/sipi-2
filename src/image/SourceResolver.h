#pragma once

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace sipi {

enum class SourceErrorCode { configuration, invalid_path, not_found, io };
/** Typed source/configuration failure; diagnostics intentionally omit local paths. */
class SourceError : public std::runtime_error {
public:
    SourceError(SourceErrorCode code, const char* message) : std::runtime_error(message), code_(code) {}
    [[nodiscard]] SourceErrorCode code() const noexcept { return code_; }
private:
    SourceErrorCode code_;
};

/** Resolve a decoded identifier or trusted preflight override to an existing file.
 * Relative sources require a configured root and stay inside its canonical path.
 * Only explicit absolute script overrides may bypass that root. Prefixes do not
 * participate in default mapping. No second URL decoding or image I/O occurs.
 * Repository directories/symlinks must not be changed by untrusted writers while
 * serving: canonicalization is not an atomic open and cannot defeat rename races.
 */
class SourceResolver {
public:
    /** Validate/canonicalize an explicitly configured directory, relative to cwd.
     * Absent root is valid for absolute script overrides; no implicit cwd root. */
    explicit SourceResolver(const std::optional<std::filesystem::path>& root = std::nullopt);
    /** Returns a canonical absolute regular-file path or throws SourceError.
     * Reject traversal/absolute identifiers and root-escaping symlinks. */
    [[nodiscard]] std::filesystem::path resolve(
        std::string_view identifier,
        const std::optional<std::filesystem::path>& override = std::nullopt) const;
private:
    std::optional<std::filesystem::path> root_;
};
} // namespace sipi
