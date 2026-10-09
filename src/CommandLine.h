#pragma once

#include "storage/S3ByteSource.h"
#include "cache/ResultCache.h"
#include "cache/MetadataCache.h"
#include "cache/MaskCache.h"
#include "cache/VariantCache.h"
#include "image/ImageSource.h"
#include "http/WorkerLimits.h"
#include "image/Bitonal.h"
#include "image/OutputLimits.h"
#include "imgformats/JpegOptions.h"
#include "preflight/ScriptConfig.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace sipi {

/** Startup action; help and version bypass environment and configuration files. */
enum class Command { run, help, version };

/** Application severity, translated to the logging backend by main. */
enum class LogLevel { trace, debug, info, warn, error, critical, off };

/** Fully resolved server settings, with defaults for absent values. */
struct ServerConfig {
    // Credentials resolve here; constructing an S3 client is a separate step.
    // Empty credentials retain the SDK provider chain. Never log this object.
    S3Config s3;
    StorageKind iiif_storage = StorageKind::local;
    std::optional<std::string> s3_bucket;
    // Absent disables the persistent cache. Relative paths use the working directory.
    std::optional<std::filesystem::path> cache_dir;
    cache::CacheLimits cache_limits;
    cache::MetadataCacheLimits metadata_cache_limits;
    cache::MaskCacheLimits mask_cache_limits;
    cache::VariantCacheLimits variant_cache_limits;
    unsigned iiif_workers = http::default_worker_count();
    BitonalMethod bitonal_method = BitonalMethod::threshold;
    LogLevel log_level = LogLevel::info;
    std::uint16_t port = 8080;
    // Numeric IPv4/IPv6 address; containers explicitly opt into 0.0.0.0.
    std::string listen_address = "127.0.0.1";
    bool size_upscaling = false;
    OutputLimits output_limits;
    JpegOptions jpeg; // Server-wide encoding settings, immutable after startup.
    // Wildcard is exclusive; explicit origins are matched exactly, never reflected blindly.
    std::vector<std::string> cors_origins{"*"};
    bool cors_credentials = false;
    // Absent disables preflight; relative paths use the process working directory.
    std::optional<std::filesystem::path> preflight_script;
    // Explicit Lua settings only; secret files are read later at startup. Never log.
    preflight::ScriptSettings preflight_settings;
    // Optional repository root; relative values use the process working directory.
    std::optional<std::filesystem::path> iiif_root;
    // Public origin and optional proxy mount path, without a trailing slash.
    // Absent uses http://127.0.0.1:<port>; request headers never override it.
    std::optional<std::string> public_base_url;
};

/** Startup action, resolved configuration, and generated help when requested. */
struct StartupOptions {
    Command command = Command::run;
    ServerConfig config;
    std::string help_text;
};

/** Owns a diagnostic independently of the input strings and file contents. */
struct ParseError {
    std::string message;
};

using ParseResult = std::variant<StartupOptions, ParseError>;

/** Caller-supplied S3 credential environment snapshot. Views live for one load.
 * nullopt means unset; a selected empty value is invalid, as for other settings.
 * These are SIPI_S3_* variables, separate from the SDK's AWS_* provider chain.
 */
struct S3CredentialEnvironment {
    std::optional<std::string_view> access_key;
    std::optional<std::string_view> secret_key;
    std::optional<std::string_view> session_token;
};

/** Optional SIPI_IIIF_STORAGE and SIPI_S3_{BUCKET,REGION,ENDPOINT,PATH_STYLE}
 * snapshots; resolution uses the same per-field priority as all other settings. */
struct RepositoryEnvironment {
    std::optional<std::string_view> storage, bucket, region, endpoint, path_style;
};

/** Caller-supplied SIPI_CACHE_{DIR,MAX_BYTES,TARGET_PERCENT} and
 * SIPI_METADATA_CACHE_{MAX_ENTRIES,MAX_BYTES} and SIPI_MASK_CACHE_{MAX_ENTRIES,MAX_BYTES},
 * SIPI_VARIANT_CACHE_{MAX_ENTRIES,MAX_BYTES} snapshots. */
struct CacheEnvironment {
    std::optional<std::string_view> directory, max_bytes, target_percent;
    std::optional<std::string_view> metadata_max_entries = std::nullopt;
    std::optional<std::string_view> metadata_max_bytes = std::nullopt;
    std::optional<std::string_view> mask_max_entries = std::nullopt;
    std::optional<std::string_view> mask_max_bytes = std::nullopt;
    std::optional<std::string_view> variant_max_entries = std::nullopt;
    std::optional<std::string_view> variant_max_bytes = std::nullopt;
};

/** Caller-owned CORS snapshots; origins is * or a comma-separated origin list. */
struct CorsEnvironment {
    std::optional<std::string_view> origins, credentials;
};

/** Caller-owned SIPI_MAX_WIDTH, SIPI_MAX_HEIGHT and SIPI_MAX_AREA snapshots. */
struct OutputLimitEnvironment {
    std::optional<std::string_view> width, height, area;
};

/** Caller-owned SIPI_JPEG_QUALITY and SIPI_JPEG_SAMPLING snapshots. */
struct JpegEnvironment {
    std::optional<std::string_view> quality, sampling;
};

/**
 * Resolve startup options in CLI > environment > TOML file > default order.
 *
 * Arguments exclude argv[0]. env_log_level is the caller's snapshot of
 * SIPI_LOG_LEVEL; nullopt means unset, an empty string is invalid if selected.
 * env_port similarly supplies SIPI_PORT; valid ports range from 1 to 65535.
 * env_size_upscaling supplies SIPI_SIZE_UPSCALING (exactly true or false).
 * env_preflight_script supplies SIPI_PREFLIGHT_SCRIPT; selected paths must be
 * nonempty and contain no NUL bytes. Script I/O belongs to server initialization.
 * env_iiif_root supplies SIPI_IIIF_ROOT, with the same path syntax validation.
 * env_public_base_url supplies SIPI_PUBLIC_BASE_URL, an absolute HTTP(S) URL
 * without credentials, query or fragment. The URL path must already be encoded.
 * s3_environment supplies SIPI_S3_ACCESS_KEY, SIPI_S3_SECRET_KEY and
 * SIPI_S3_SESSION_TOKEN. Resolve each field independently, then require the
 * access/secret pair; a session token requires that pair. Help/version validate
 * supplied CLI field syntax but skip pair checks and environment/file loading.
 * env_iiif_workers supplies SIPI_IIIF_WORKERS (1..32); absent defaults to detected
 * logical CPU count capped at four, with a fallback of one.
 * env_bitonal_method supplies SIPI_BITONAL_METHOD (threshold or floyd-steinberg).
 * output_environment supplies SIPI_MAX_WIDTH/HEIGHT/AREA; zero and values above
 * OutputLimits ceilings are rejected. Absent height inherits the resolved width.
 * cors_environment supplies SIPI_CORS_ORIGINS/CREDENTIALS. Origins is * or a
 * comma-separated list of exact HTTP(S) origins; credentials requires a list.
 * Help/version skip the cross-field check, as with S3 credentials.
 * preflight_environment contains selected SIPI_PREFLIGHT_{CONFIG,SECRETS,SECRET_FILES}_*
 * NAME=VALUE entries. Merge per name; secret file I/O is deferred to startup.
 * cache_environment supplies the optional cache directory and capacity settings.
 * Parsing validates syntax only; persistent cache initialization belongs to startup.
 * jpeg_environment supplies quality (1..100) and sampling (4:4:4 or 4:2:0).
 * Input views need only live for this call. The returned result owns its data.
 * Reads only an explicitly requested --config file; does not print, log, mutate
 * the environment, or set logging levels. CLI errors are checked even for help
 * and version, which otherwise skip environment/file loading.
 *
 * Invalid input or an unreadable config file returns ParseError. Allocation
 * failures can throw. See docs/command-line.md for syntax and validation policy.
 */
[[nodiscard]] ParseResult load_startup_options(
    std::span<const std::string_view> arguments,
    std::optional<std::string_view> env_log_level = std::nullopt,
    std::optional<std::string_view> env_port = std::nullopt,
    std::optional<std::string_view> env_size_upscaling = std::nullopt,
    std::optional<std::string_view> env_preflight_script = std::nullopt,
    std::optional<std::string_view> env_iiif_root = std::nullopt,
    std::optional<std::string_view> env_public_base_url = std::nullopt,
    S3CredentialEnvironment s3_environment = {},
    RepositoryEnvironment repository_environment = {},
    CacheEnvironment cache_environment = {},
    std::optional<std::string_view> env_iiif_workers = std::nullopt,
    std::optional<std::string_view> env_bitonal_method = std::nullopt,
    OutputLimitEnvironment output_environment = {},
    CorsEnvironment cors_environment = {},
    std::span<const std::string_view> preflight_environment = {},
    std::optional<std::string_view> env_listen_address = std::nullopt,
    JpegEnvironment jpeg_environment = {});

} // namespace sipi
