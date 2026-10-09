#include "CommandLine.h"
#include "HttpServer.h"
#include "image/SourceResolver.h"
#include "preflight/LuaPreflight.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <iostream>
#include <vector>

// POSIX process environment; snapshot selected names before starting any threads.
extern char** environ;

namespace {

/** Map application log severity to spdlog without relying on enum numeric values. */
spdlog::level::level_enum logging_level(sipi::LogLevel level) {
    switch (level) {
    case sipi::LogLevel::trace: return spdlog::level::trace;
    case sipi::LogLevel::debug: return spdlog::level::debug;
    case sipi::LogLevel::info: return spdlog::level::info;
    case sipi::LogLevel::warn: return spdlog::level::warn;
    case sipi::LogLevel::error: return spdlog::level::err;
    case sipi::LogLevel::critical: return spdlog::level::critical;
    case sipi::LogLevel::off: return spdlog::level::off;
    }
    return spdlog::level::info;
}

} // namespace

/** Resolve startup settings, handle informational actions, then run the HTTP server. */
int main(int argc, char* argv[]) {
    const std::vector<std::string_view> arguments(argv + 1, argv + argc);
    std::optional<std::string_view> env_log_level;
    if (const char* value = std::getenv("SIPI_LOG_LEVEL")) {
        env_log_level = value;
    }
    std::optional<std::string_view> env_port;
    if (const char* value = std::getenv("SIPI_PORT")) {
        env_port = value;
    }
    std::optional<std::string_view> env_size_upscaling;
    if (const char* value = std::getenv("SIPI_SIZE_UPSCALING")) {
        env_size_upscaling = value;
    }
    std::optional<std::string_view> env_preflight_script;
    if (const char* value = std::getenv("SIPI_PREFLIGHT_SCRIPT")) {
        env_preflight_script = value;
    }
    std::optional<std::string_view> env_iiif_root;
    if (const char* value = std::getenv("SIPI_IIIF_ROOT")) env_iiif_root = value;
    std::optional<std::string_view> env_public_base_url;
    if (const char* value = std::getenv("SIPI_PUBLIC_BASE_URL")) env_public_base_url = value;
    sipi::S3CredentialEnvironment s3_environment;
    if (const char* value = std::getenv("SIPI_S3_ACCESS_KEY")) s3_environment.access_key = value;
    if (const char* value = std::getenv("SIPI_S3_SECRET_KEY")) s3_environment.secret_key = value;
    if (const char* value = std::getenv("SIPI_S3_SESSION_TOKEN")) s3_environment.session_token = value;
    sipi::RepositoryEnvironment repository_environment;
    if (const char* value = std::getenv("SIPI_IIIF_STORAGE")) repository_environment.storage = value;
    if (const char* value = std::getenv("SIPI_S3_BUCKET")) repository_environment.bucket = value;
    if (const char* value = std::getenv("SIPI_S3_REGION")) repository_environment.region = value;
    if (const char* value = std::getenv("SIPI_S3_ENDPOINT")) repository_environment.endpoint = value;
    if (const char* value = std::getenv("SIPI_S3_PATH_STYLE")) repository_environment.path_style = value;
    sipi::CacheEnvironment cache_environment;
    if (const char* value = std::getenv("SIPI_CACHE_DIR")) cache_environment.directory = value;
    if (const char* value = std::getenv("SIPI_CACHE_MAX_BYTES")) cache_environment.max_bytes = value;
    if (const char* value = std::getenv("SIPI_CACHE_TARGET_PERCENT")) cache_environment.target_percent = value;
    if (const char* value = std::getenv("SIPI_METADATA_CACHE_MAX_ENTRIES")) cache_environment.metadata_max_entries = value;
    if (const char* value = std::getenv("SIPI_METADATA_CACHE_MAX_BYTES")) cache_environment.metadata_max_bytes = value;
    if (const char* value = std::getenv("SIPI_MASK_CACHE_MAX_ENTRIES")) cache_environment.mask_max_entries = value;
    if (const char* value = std::getenv("SIPI_MASK_CACHE_MAX_BYTES")) cache_environment.mask_max_bytes = value;
    if (const char* value = std::getenv("SIPI_VARIANT_CACHE_MAX_ENTRIES")) cache_environment.variant_max_entries = value;
    if (const char* value = std::getenv("SIPI_VARIANT_CACHE_MAX_BYTES")) cache_environment.variant_max_bytes = value;
    std::optional<std::string_view> env_iiif_workers;
    if (const char* value = std::getenv("SIPI_IIIF_WORKERS")) env_iiif_workers = value;
    std::optional<std::string_view> env_bitonal_method;
    if (const char* value = std::getenv("SIPI_BITONAL_METHOD")) env_bitonal_method = value;
    sipi::OutputLimitEnvironment output_environment;
    if (const char* value = std::getenv("SIPI_MAX_WIDTH")) output_environment.width = value;
    if (const char* value = std::getenv("SIPI_MAX_HEIGHT")) output_environment.height = value;
    if (const char* value = std::getenv("SIPI_MAX_AREA")) output_environment.area = value;
    sipi::CorsEnvironment cors_environment;
    if (const char* value = std::getenv("SIPI_CORS_ORIGINS")) cors_environment.origins = value;
    if (const char* value = std::getenv("SIPI_CORS_CREDENTIALS")) cors_environment.credentials = value;
    std::vector<std::string_view> preflight_environment;
    for (char** entry = environ; entry && *entry; ++entry) {
        const std::string_view text(*entry);
        if (text.starts_with("SIPI_PREFLIGHT_CONFIG_") || text.starts_with("SIPI_PREFLIGHT_SECRETS_") ||
            text.starts_with("SIPI_PREFLIGHT_SECRET_FILES_"))
            preflight_environment.push_back(text);
    }
    sipi::JpegEnvironment jpeg_environment;
    if (const char* value = std::getenv("SIPI_JPEG_QUALITY")) jpeg_environment.quality = value;
    if (const char* value = std::getenv("SIPI_JPEG_SAMPLING")) jpeg_environment.sampling = value;
    std::optional<std::string_view> env_listen_address;
    if (const char* value = std::getenv("SIPI_LISTEN_ADDRESS")) env_listen_address = value;
    const auto parsed = sipi::load_startup_options(arguments, env_log_level, env_port,
        env_size_upscaling, env_preflight_script, env_iiif_root, env_public_base_url, s3_environment, repository_environment, cache_environment, env_iiif_workers, env_bitonal_method, output_environment, cors_environment, preflight_environment, env_listen_address, jpeg_environment);
    if (const auto* error = std::get_if<sipi::ParseError>(&parsed)) {
        std::cerr << "Configuration error: " << error->message << '\n';
        return 2;
    }
    const auto& options = std::get<sipi::StartupOptions>(parsed);
    if (options.command == sipi::Command::help) {
        std::cout << options.help_text;
        return 0;
    }
    if (options.command == sipi::Command::version) {
        std::cout << "SIPI-2 " << SIPI_VERSION << '\n';
        return 0;
    }
    spdlog::set_level(logging_level(options.config.log_level));
    // Validate before listening, then transfer the snapshot to the HTTP worker.
    std::optional<sipi::preflight::LuaPreflight> preflight;
    if (options.config.preflight_script) {
        try {
            auto script_config = sipi::preflight::resolve_script_config(options.config.preflight_settings);
            preflight.emplace(*options.config.preflight_script, std::move(script_config));
            preflight->validate();
        } catch (const sipi::preflight::ScriptConfigError& error) {
            // This exception contains only fixed diagnostics, never paths/values.
            std::cerr << "Preflight configuration error: " << error.what() << '\n';
            return 2;
        } catch (const sipi::preflight::PreflightError& error) {
            // Lua diagnostics can include secrets via error(), assert() or source
            // snippets. Keep server logs value-free, as in the request worker.
            std::cerr << "Preflight startup error: category " << static_cast<int>(error.code())
                      << "; check script syntax, configuration and entry point\n";
            return 2;
        }
        spdlog::info("Preflight script validated");
    }
    // Prepare/recover persistent storage and retain its exclusive directory lock.
    std::shared_ptr<sipi::cache::ResultCache> cache;
    if (options.config.cache_dir) {
        try {
            cache = std::make_shared<sipi::cache::ResultCache>(*options.config.cache_dir, options.config.cache_limits);
            spdlog::info("Persistent cache initialized");
        } catch (const sipi::cache::CacheError&) {
            spdlog::warn("Persistent cache unavailable; continuing without cache");
        } catch (const std::filesystem::filesystem_error&) {
            spdlog::warn("Persistent cache filesystem unavailable; continuing without cache");
        }
    }
    try {
        sipi::run_http_server(options.config, std::move(preflight), std::move(cache));
    } catch (const sipi::ByteSourceError&) {
        std::cerr << "S3 startup error: invalid backend configuration\n";
        return 2;
    } catch (const sipi::SourceError& error) {
        std::cerr << "Image repository startup error: " << error.what() << '\n';
        return 2;
    }
    return 0;
}
