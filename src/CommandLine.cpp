#include "CommandLine.h"

#include <CLI/CLI.hpp>

#include <fstream>
#include <map>
#include <charconv>
#include <regex>
#include <arpa/inet.h>
#include <vector>

namespace sipi {
namespace {

/** Validate an operator-supplied public origin/mount without network access.
 * Accept ASCII DNS names, IPv4 or bracketed IPv6, and an already encoded path.
 * No credentials/query/fragment; dot segments and empty path segments would
 * change identity when a client normalizes the resulting service URL.
 */
std::string validate_public_base_url(const std::string& value) {
    static const std::regex syntax{
        R"(^https?://([A-Za-z0-9.-]+|\[[0-9A-Fa-f:.]+\])(:([0-9]+))?(/[A-Za-z0-9._~!$&'()*+,;=:@%/-]*)?$)"};
    std::smatch match;
    if (!std::regex_match(value, match, syntax))
        return "Expected an absolute HTTP(S) URL without credentials, query or fragment";
    const auto host = match[1].str();
    if (host.front() == '[') {
        in6_addr address{};
        if (inet_pton(AF_INET6, host.substr(1, host.size() - 2).c_str(), &address) != 1)
            return "Invalid IPv6 address in public base URL";
    } else {
        static const std::regex hostname{R"(^[A-Za-z0-9]([A-Za-z0-9-]*[A-Za-z0-9])?(\.[A-Za-z0-9]([A-Za-z0-9-]*[A-Za-z0-9])?)*$)"};
        if (!std::regex_match(host, hostname)) return "Invalid hostname in public base URL";
    }
    if (match[3].matched) {
        const auto port = match[3].str();
        unsigned number = 0;
        const auto parsed = std::from_chars(port.data(), port.data() + port.size(), number);
        if (parsed.ec != std::errc{} || number == 0 || number > 65535)
            return "Public base URL port must be between 1 and 65535";
    }
    const auto path = match[4].str();
    std::string segment;
    for (std::size_t i = 1; i < path.size(); ++i) {
        if (path[i] == '/') {
            if (segment.empty() || segment == "." || segment == "..")
                return "Public base URL must not contain empty or dot path segments";
            segment.clear();
        } else if (path[i] == '%') {
            if (i + 2 >= path.size()) return "Invalid percent escape in public base URL";
            unsigned byte = 0;
            const auto parsed = std::from_chars(path.data() + i + 1, path.data() + i + 3, byte, 16);
            if (parsed.ec != std::errc{} || parsed.ptr != path.data() + i + 3 || byte <= 0x1f || byte == 0x7f)
                return "Invalid percent escape in public base URL";
            segment += static_cast<char>(byte);
            i += 2;
        } else {
            segment += path[i];
        }
    }
    if (segment == "." || segment == "..") return "Public base URL must not contain dot path segments";
    return {};
}

} // namespace

ParseResult load_startup_options(std::span<const std::string_view> arguments,
                                std::optional<std::string_view> env_log_level,
                                std::optional<std::string_view> env_port,
                                std::optional<std::string_view> env_size_upscaling,
                                std::optional<std::string_view> env_preflight_script,
                                std::optional<std::string_view> env_iiif_root,
                                std::optional<std::string_view> env_public_base_url) {
    StartupOptions result;
    bool help = false;
    bool version = false;
    std::string config_path;
    std::string preflight_script;
    std::string iiif_root;
    std::string public_base_url;
    std::string log_level = "info";
    std::string size_upscaling = "false";
    CLI::App app{"SIPI-2 IIIF server", "sipi-2"};
    // Ordinary flags preserve our policy: validate all CLI input before help/version.
    app.set_help_flag();
    auto* help_option = app.add_flag("--help", help, "Show usage")
                            ->disable_flag_override()->configurable(false)
                            ->multi_option_policy(CLI::MultiOptionPolicy::Throw);
    auto* version_option = app.add_flag("--version", version, "Show version")
                               ->disable_flag_override()->configurable(false)
                               ->multi_option_policy(CLI::MultiOptionPolicy::Throw);
    help_option->excludes(version_option);
    auto* config_option = app.add_option("--config", config_path, "Read a TOML configuration file")
                              ->configurable(false);

    const std::map<std::string, LogLevel> levels{
        {"trace", LogLevel::trace}, {"debug", LogLevel::debug}, {"info", LogLevel::info},
        {"warn", LogLevel::warn}, {"error", LogLevel::error},
        {"critical", LogLevel::critical}, {"off", LogLevel::off}};
    auto* log_option = app.add_option("--log-level", log_level,
                                     "Log severity (environment: SIPI_LOG_LEVEL; default: info)")
                           ->check(CLI::IsMember(levels))->type_name("LEVEL");
    auto* port_option = app.add_option("--port", result.config.port,
                                      "HTTP port (environment: SIPI_PORT; default: 8080)")
                            ->check(CLI::Range(1, 65535));
    auto* upscale_option = app.add_option("--size-upscaling", size_upscaling,
                                          "Allow IIIF enlargement (SIPI_SIZE_UPSCALING; default: false)")
                               ->check(CLI::IsMember({"true", "false"}))->type_name("BOOL");
    const auto path_validator = CLI::Validator([](const std::string& value) {
        return value.empty() || value.find('\0') != std::string::npos
            ? std::string{"Path must be nonempty and contain no NUL bytes"} : std::string{};
    }, "PATH");
    auto* preflight_option = app.add_option("--preflight-script", preflight_script,
        "Preflight Lua file (SIPI_PREFLIGHT_SCRIPT; default: disabled)")
        ->check(path_validator)->type_name("PATH");
    auto* root_option = app.add_option("--iiif-root", iiif_root,
        "Image repository root (SIPI_IIIF_ROOT; default: not configured)")
        ->check(path_validator)->type_name("PATH");
    auto* public_url_option = app.add_option("--public-base-url", public_base_url,
        "Public origin and proxy mount (SIPI_PUBLIC_BASE_URL; default: local listener)")
        ->check(CLI::Validator(validate_public_base_url, "URL"))->type_name("URL");
    const auto save_public_url = [&] {
        if (public_url_option->empty()) return;
        if (public_base_url.ends_with('/')) public_base_url.pop_back();
        result.config.public_base_url = public_base_url;
    };
    app.allow_config_extras(CLI::config_extras_mode::error);

    // CLI11's vector overload consumes arguments from the back.
    std::vector<std::string> tokens(arguments.rbegin(), arguments.rend());
    try {
        app.parse(tokens);
        if (help || version) {
            result.config.log_level = levels.at(log_level);
            result.config.size_upscaling = size_upscaling == "true";
            if (!preflight_option->empty()) result.config.preflight_script = preflight_script;
            if (!root_option->empty()) result.config.iiif_root = iiif_root;
            save_public_url();
            result.command = help ? Command::help : Command::version;
            if (help) {
                result.help_text = app.help();
            }
            return result;
        }

        // Load higher-priority sources first. CLI11's config reader fills only
        // options with no result. set_config()/envname() use a different order.
        if (log_option->empty() && env_log_level.has_value()) {
            if (env_log_level->empty()) {
                return ParseError{"SIPI_LOG_LEVEL must not be empty"};
            }
            log_option->add_result(std::string{*env_log_level});
            log_option->run_callback();
        }
        if (port_option->empty() && env_port.has_value()) {
            if (env_port->empty()) {
                return ParseError{"SIPI_PORT must not be empty"};
            }
            port_option->add_result(std::string{*env_port});
            port_option->run_callback();
        }
        if (upscale_option->empty() && env_size_upscaling.has_value()) {
            if (env_size_upscaling->empty()) {
                return ParseError{"SIPI_SIZE_UPSCALING must not be empty"};
            }
            upscale_option->add_result(std::string{*env_size_upscaling});
            upscale_option->run_callback();
        }
        if (preflight_option->empty() && env_preflight_script.has_value()) {
            if (env_preflight_script->empty()) {
                return ParseError{"SIPI_PREFLIGHT_SCRIPT must not be empty"};
            }
            preflight_option->add_result(std::string{*env_preflight_script});
            preflight_option->run_callback();
        }
        if (root_option->empty() && env_iiif_root.has_value()) {
            if (env_iiif_root->empty()) return ParseError{"SIPI_IIIF_ROOT must not be empty"};
            root_option->add_result(std::string{*env_iiif_root});
            root_option->run_callback();
        }
        if (public_url_option->empty() && env_public_base_url.has_value()) {
            if (env_public_base_url->empty()) return ParseError{"SIPI_PUBLIC_BASE_URL must not be empty"};
            public_url_option->add_result(std::string{*env_public_base_url});
            public_url_option->run_callback();
        }
        if (!config_option->empty()) {
            std::ifstream input{config_path};
            if (!input) {
                return ParseError{"Cannot open configuration file: " + config_path};
            }
            app.parse_from_stream(input);
            if (input.bad()) {
                return ParseError{"Cannot read configuration file: " + config_path};
            }
        }
    } catch (const CLI::ParseError& error) {
        return ParseError{error.what()};
    }
    if (!preflight_option->empty()) result.config.preflight_script = preflight_script;
    if (!root_option->empty()) result.config.iiif_root = iiif_root;
    save_public_url();
    result.config.log_level = levels.at(log_level);
    result.config.size_upscaling = size_upscaling == "true";
    return result;
}

} // namespace sipi
