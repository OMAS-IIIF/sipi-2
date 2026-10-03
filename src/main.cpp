#include "CommandLine.h"
#include "HttpServer.h"
#include "image/SourceResolver.h"
#include "preflight/LuaPreflight.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <iostream>
#include <vector>

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
    const auto parsed = sipi::load_startup_options(arguments, env_log_level, env_port,
        env_size_upscaling, env_preflight_script, env_iiif_root, env_public_base_url);
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
            preflight.emplace(*options.config.preflight_script);
            preflight->validate();
        } catch (const sipi::preflight::PreflightError& error) {
            std::cerr << "Preflight startup error: " << error.what() << '\n';
            return 2;
        }
        spdlog::info("Preflight script validated");
    }
    try {
        sipi::run_http_server(options.config, std::move(preflight));
    } catch (const sipi::SourceError& error) {
        std::cerr << "Image repository startup error: " << error.what() << '\n';
        return 2;
    }
    return 0;
}
