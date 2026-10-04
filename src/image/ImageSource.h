#pragma once
#include <filesystem>
#include <string>
#include <variant>

namespace sipi {
/** Default identifier mapping. IIIF URL prefixes never participate. */
enum class StorageKind { local, s3 };
/** Owning source selected after authorization; no credentials or open handles. */
struct LocalSource { std::filesystem::path path; };
struct S3Source { std::string bucket; std::string key; };
using ImageSource = std::variant<LocalSource, S3Source>;
/** Trusted Lua override: a local path or an object key in the configured bucket.
 * A string path remains local even when the default backend is S3. */
struct S3Key { std::string key; bool operator==(const S3Key&) const = default; };
using SourceOverride = std::variant<std::filesystem::path, S3Key>;
} // namespace sipi
