#include "image/SourceResolver.h"

namespace sipi {
namespace {
/** Compare path components, not string prefixes (images-other is not images). */
bool inside(const std::filesystem::path& root, const std::filesystem::path& path) {
    auto part = path.begin();
    for (const auto& component : root) {
        if (part == path.end() || component != *part++) return false;
    }
    return true;
}

void filesystem_error(const std::error_code& error) {
    if (error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory)
        throw SourceError(SourceErrorCode::not_found, "Image source was not found");
    throw SourceError(SourceErrorCode::io, "Image source could not be accessed");
}
} // namespace

SourceResolver::SourceResolver(const std::optional<std::filesystem::path>& root) {
    if (!root) return;
    if (root->empty() || root->string().find('\0') != std::string::npos)
        throw SourceError(SourceErrorCode::configuration, "iiif-root must name an existing directory");
    std::error_code error;
    auto canonical = std::filesystem::canonical(*root, error);
    if (error ||
        !std::filesystem::is_directory(canonical, error) || error)
        throw SourceError(SourceErrorCode::configuration, "iiif-root must name an existing directory");
    root_ = std::move(canonical);
}

std::filesystem::path SourceResolver::resolve(
    std::string_view identifier, const std::optional<std::filesystem::path>& override) const {
    const std::filesystem::path selected = override ? *override : std::filesystem::path(identifier);
    const auto text = selected.string();
    if (text.empty() || text.find('\0') != std::string::npos)
        throw SourceError(SourceErrorCode::invalid_path, "Invalid image source name");
    const bool absolute_override = override && selected.is_absolute();
    if (!absolute_override) {
        if (selected.has_root_path() || text.find('\\') != std::string::npos)
            throw SourceError(SourceErrorCode::invalid_path, "Invalid relative image source name");
        for (const auto& part : selected) {
            if (part == "..")
                throw SourceError(SourceErrorCode::invalid_path, "Image source traversal is not allowed");
        }
        if (!root_)
            throw SourceError(SourceErrorCode::configuration, "No iiif-root is configured for relative image sources");
    }
    const auto candidate = absolute_override ? selected : *root_ / selected;
    std::error_code error;
    const auto canonical = std::filesystem::canonical(candidate, error);
    if (error) filesystem_error(error);
    if (!absolute_override && !inside(*root_, canonical))
        throw SourceError(SourceErrorCode::invalid_path, "Image source leaves the configured repository");
    const auto status = std::filesystem::status(canonical, error);
    if (error) filesystem_error(error);
    if (!std::filesystem::is_regular_file(status))
        throw SourceError(SourceErrorCode::not_found, "Image source was not found");
    return canonical;
}
} // namespace sipi
