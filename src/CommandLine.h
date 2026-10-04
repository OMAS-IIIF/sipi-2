#pragma once

#include "storage/S3ByteSource.h"
#include "image/ImageSource.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

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
    LogLevel log_level = LogLevel::info;
    std::uint16_t port = 8080;
    bool size_upscaling = false;
    // Absent disables preflight; relative paths use the process working directory.
    std::optional<std::filesystem::path> preflight_script;
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
    RepositoryEnvironment repository_environment = {});

} // namespace sipi
