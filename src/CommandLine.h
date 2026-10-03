#pragma once

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
    std::optional<std::string_view> env_public_base_url = std::nullopt);

} // namespace sipi
