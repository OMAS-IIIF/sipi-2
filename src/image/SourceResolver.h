#pragma once

#include "ImageSource.h"
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
    /** Select a typed source; only the identifier participates in default mapping.
     * S3 keys remain literal, without filesystem normalization or URL decoding.
     * Local selections retain the existing root-containment checks. */
    [[nodiscard]] ImageSource resolve_source(std::string_view identifier,
        const std::optional<SourceOverride>& override = std::nullopt) const;
private:
    StorageKind storage_;
    std::optional<std::string> bucket_;
    SourceErrorCode code_;
};

/** Resolve a decoded identifier or trusted preflight override to a typed source.
 * Local selections use the existing file/containment checks below; S3 selections
 * preserve a literal key in the configured bucket without network access.
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
    explicit SourceResolver(const std::optional<std::filesystem::path>& root = std::nullopt,
                            StorageKind storage = StorageKind::local,
                            std::optional<std::string> bucket = std::nullopt);
    /** Returns a canonical absolute regular-file path or throws SourceError.
     * Reject traversal/absolute identifiers and root-escaping symlinks. */
    [[nodiscard]] std::filesystem::path resolve(
        std::string_view identifier,
        const std::optional<std::filesystem::path>& override = std::nullopt) const;
    /** Select a typed source; only the identifier participates in default mapping.
     * S3 keys remain literal, without filesystem normalization or URL decoding.
     * Local selections retain the existing root-containment checks. */
    [[nodiscard]] ImageSource resolve_source(std::string_view identifier,
        const std::optional<SourceOverride>& override = std::nullopt) const;
private:
    StorageKind storage_;
    std::optional<std::string> bucket_;
    std::optional<std::filesystem::path> root_;
};
} // namespace sipi
