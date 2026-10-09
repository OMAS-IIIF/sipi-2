#include "CommandLine.h"

#include <CLI/CLI.hpp>

#include <fstream>
#include <limits>
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
                                std::optional<std::string_view> env_public_base_url,
                                S3CredentialEnvironment s3_environment,
                                RepositoryEnvironment repository_environment,
                                CacheEnvironment cache_environment,
                                std::optional<std::string_view> env_iiif_workers,
                                std::optional<std::string_view> env_bitonal_method,
                                OutputLimitEnvironment output_environment,
                                CorsEnvironment cors_environment,
                                std::span<const std::string_view> preflight_environment,
                                std::optional<std::string_view> env_listen_address,
                                JpegEnvironment jpeg_environment) {
    StartupOptions result;
    bool help = false;
    bool version = false;
    std::string config_path;
    std::string preflight_script;
    std::vector<std::string> script_config, script_secrets, script_secret_files;
    std::string iiif_root;
    std::string cache_dir;
    std::string public_base_url;
    std::string storage = "local", bucket, path_style = "false";
    std::string log_level = "info";
    std::string bitonal_method = "threshold";
    std::string jpeg_sampling = "4:4:4";
    std::string size_upscaling = "false";
    std::string cors_origins = "*", cors_credentials = "false";
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
    auto* listen_option = app.add_option("--listen-address", result.config.listen_address,
        "Numeric bind address (SIPI_LISTEN_ADDRESS; default: 127.0.0.1)")
        ->check(CLI::Validator([](const std::string& value) {
            in_addr ipv4{};
            in6_addr ipv6{};
            return value.find('\0') == std::string::npos &&
                (inet_pton(AF_INET, value.c_str(), &ipv4) == 1 ||
                 inet_pton(AF_INET6, value.c_str(), &ipv6) == 1)
                ? std::string{} : std::string{"Expected a numeric IPv4 or IPv6 address"};
        }, "IP"));
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
    auto* script_config_option = app.add_option("--preflight-config", script_config,
        "Lua setting NAME=VALUE (repeatable; SIPI_PREFLIGHT_CONFIG_<NAME>)")
        ->expected(1)->multi_option_policy(CLI::MultiOptionPolicy::TakeAll)->type_name("NAME=VALUE");
    auto* script_secret_option = app.add_option("--preflight-secret", script_secrets,
        "Lua secret NAME=VALUE (prefer environment/file; SIPI_PREFLIGHT_SECRETS_<NAME>)")
        ->expected(1)->multi_option_policy(CLI::MultiOptionPolicy::TakeAll)->type_name("NAME=VALUE");
    auto* script_file_option = app.add_option("--preflight-secret-file", script_secret_files,
        "Lua secret NAME=PATH (repeatable; SIPI_PREFLIGHT_SECRET_FILES_<NAME>)")
        ->expected(1)->multi_option_policy(CLI::MultiOptionPolicy::TakeAll)->type_name("NAME=PATH");
    auto* root_option = app.add_option("--iiif-root", iiif_root,
        "Image repository root (SIPI_IIIF_ROOT; default: not configured)")
        ->check(path_validator)->type_name("PATH");
    auto* cache_dir_option = app.add_option("--cache-dir", cache_dir,
        "Persistent result cache directory (SIPI_CACHE_DIR; default: disabled)")
        ->check(path_validator)->type_name("PATH");
    const auto unsigned_decimal = CLI::Validator([](const std::string& value) {
        return value.empty() || value.find_first_not_of("0123456789") != std::string::npos
            ? std::string{"Expected unsigned decimal integer"} : std::string{};
    }, "INTEGER");
    auto* width_option = app.add_option("--max-width", result.config.output_limits.max_width,
        "Output width limit (SIPI_MAX_WIDTH; default: 8192; ceiling: 32768)")
        ->check(unsigned_decimal)->check(CLI::Range(1u, OutputLimits::axis_ceiling));
    auto* height_option = app.add_option("--max-height", result.config.output_limits.max_height,
        "Output height limit (SIPI_MAX_HEIGHT; default: max-width; ceiling: 32768)")
        ->check(unsigned_decimal)->check(CLI::Range(1u, OutputLimits::axis_ceiling));
    auto* area_option = app.add_option("--max-area", result.config.output_limits.max_area,
        "Output pixel area limit (SIPI_MAX_AREA; default/ceiling: 8388608)")
        ->check(unsigned_decimal)->check(CLI::Range(std::uint64_t{1}, OutputLimits::area_ceiling));
    auto* jpeg_quality_option = app.add_option("--jpeg-quality", result.config.jpeg.quality,
        "JPEG quality (SIPI_JPEG_QUALITY; default: 90; range: 1..100)")
        ->check(unsigned_decimal)->check(CLI::Range(1, 100));
    auto* jpeg_sampling_option = app.add_option("--jpeg-sampling", jpeg_sampling,
        "JPEG chroma sampling (SIPI_JPEG_SAMPLING; default: 4:4:4)")
        ->check(CLI::IsMember({"4:4:4", "4:2:0"}));
    auto* bitonal_option = app.add_option("--bitonal-method", bitonal_method,
        "Bitonal method (SIPI_BITONAL_METHOD; default: threshold)")
        ->check(CLI::IsMember({"threshold", "floyd-steinberg"}));
    auto* workers_option = app.add_option("--iiif-workers", result.config.iiif_workers,
        "IIIF workers (SIPI_IIIF_WORKERS; default: CPU count capped at 4; range: 1..32)")
        ->check(unsigned_decimal)->check(CLI::Range(1u, http::maximum_worker_count));
    auto* cache_max_option = app.add_option("--cache-max-bytes", result.config.cache_limits.max_bytes,
        "Encoded cache byte limit (SIPI_CACHE_MAX_BYTES; default: 1073741824)")
        ->check(unsigned_decimal)->check(CLI::Range(std::uint64_t{1}, std::uint64_t{INT64_MAX}/2));
    auto* cache_target_option = app.add_option("--cache-target-percent", result.config.cache_limits.target_percent,
        "Cleanup target percent (SIPI_CACHE_TARGET_PERCENT; default: 90)")
        ->check(unsigned_decimal)->check(CLI::Range(1,99));
    auto* metadata_entries_option = app.add_option("--metadata-cache-max-entries",
        result.config.metadata_cache_limits.max_entries,
        "Metadata cache entry limit (SIPI_METADATA_CACHE_MAX_ENTRIES; default: 256; 0 disables)")
        ->check(unsigned_decimal)->check(CLI::Range(std::size_t{0}, std::numeric_limits<std::size_t>::max()));
    auto* metadata_bytes_option = app.add_option("--metadata-cache-max-bytes",
        result.config.metadata_cache_limits.max_bytes,
        "Metadata cache byte budget (SIPI_METADATA_CACHE_MAX_BYTES; default: 67108864; 0 disables)")
        ->check(unsigned_decimal)->check(CLI::Range(std::size_t{0}, std::numeric_limits<std::size_t>::max()));
    auto* mask_entries_option = app.add_option("--mask-cache-max-entries",
        result.config.mask_cache_limits.max_entries,
        "Mask cache entry limit (SIPI_MASK_CACHE_MAX_ENTRIES; default: 64; 0 disables)")
        ->check(unsigned_decimal)->check(CLI::Range(std::size_t{0}, std::numeric_limits<std::size_t>::max()));
    auto* mask_bytes_option = app.add_option("--mask-cache-max-bytes",
        result.config.mask_cache_limits.max_bytes,
        "Mask cache byte budget (SIPI_MASK_CACHE_MAX_BYTES; default: 33554432; 0 disables)")
        ->check(unsigned_decimal)->check(CLI::Range(std::size_t{0}, std::numeric_limits<std::size_t>::max()));
    auto* variant_entries_option = app.add_option("--variant-cache-max-entries",
        result.config.variant_cache_limits.max_entries,
        "Variant cache entry limit (SIPI_VARIANT_CACHE_MAX_ENTRIES; default: 16; 0 disables)")
        ->check(unsigned_decimal)->check(CLI::Range(std::size_t{0}, std::numeric_limits<std::size_t>::max()));
    auto* variant_bytes_option = app.add_option("--variant-cache-max-bytes",
        result.config.variant_cache_limits.max_bytes,
        "Variant cache byte budget (SIPI_VARIANT_CACHE_MAX_BYTES; default: 67108864; 0 disables)")
        ->check(unsigned_decimal)->check(CLI::Range(std::size_t{0}, std::numeric_limits<std::size_t>::max()));
    auto* public_url_option = app.add_option("--public-base-url", public_base_url,
        "Public origin and proxy mount (SIPI_PUBLIC_BASE_URL; default: local listener)")
        ->check(CLI::Validator(validate_public_base_url, "URL"))->type_name("URL");
    auto* origins_option = app.add_option("--cors-origins", cors_origins,
        "CORS origins: * or comma-separated HTTP(S) origins (SIPI_CORS_ORIGINS)")
        ->check(CLI::Validator([](const std::string& value) {
            if (value == "*") return std::string{};
            std::size_t begin = 0;
            do {
                const auto end = value.find(',', begin);
                const auto origin = value.substr(begin, end - begin);
                if (!validate_public_base_url(origin).empty() ||
                    origin.find('/', origin.find("://") + 3) != std::string::npos)
                    return std::string{"Expected * or comma-separated HTTP(S) origins without paths"};
                if (end == std::string::npos) break;
                begin = end + 1;
            } while (true);
            return std::string{};
        }, "ORIGINS"));
    auto* credentials_option = app.add_option("--cors-credentials", cors_credentials,
        "Allow cross-origin cookies with explicit origins (SIPI_CORS_CREDENTIALS; default: false)")
        ->check(CLI::IsMember({"true", "false"}))->type_name("BOOL");
    auto* storage_option = app.add_option("--iiif-storage", storage,
        "Default repository: local or s3 (SIPI_IIIF_STORAGE; default: local)")
        ->check(CLI::IsMember({"local", "s3"}));
    auto* bucket_option = app.add_option("--s3-bucket", bucket,
        "Repository bucket (SIPI_S3_BUCKET; required for S3 sources)")
        ->check(CLI::Validator([](const std::string& value) {
            static const std::regex pattern{R"(^[a-z0-9]([a-z0-9.-]*[a-z0-9])?$)"};
            return value.size() < 3 || value.size() > 63 || !std::regex_match(value, pattern) ||
                value.find("..") != std::string::npos || value.find(".-") != std::string::npos ||
                value.find("-.") != std::string::npos
                ? std::string{"Invalid S3 bucket name"} : std::string{};
        }, "BUCKET"));
    auto* region_option = app.add_option("--s3-region", result.config.s3.region,
        "S3 signing region (SIPI_S3_REGION; default: us-east-1)")
        ->check(CLI::Validator([](const std::string& value) {
            static const std::regex pattern{R"(^[a-z0-9]+(-[a-z0-9]+)*$)"};
            return std::regex_match(value, pattern) ? std::string{} : std::string{"Invalid S3 region"};
        }, "REGION"));
    auto* endpoint_option = app.add_option("--s3-endpoint", result.config.s3.endpoint,
        "S3 HTTP(S) origin (SIPI_S3_ENDPOINT; default: AWS regional endpoint)")
        ->check(CLI::Validator([](const std::string& value) {
            const auto error = validate_public_base_url(value);
            if (!error.empty()) return error;
            return value.find('/', value.find("://") + 3) == std::string::npos
                ? std::string{} : std::string{"S3 endpoint must not contain a path"};
        }, "ORIGIN"));
    auto* path_style_option = app.add_option("--s3-path-style", path_style,
        "Use path-style S3 addressing (SIPI_S3_PATH_STYLE; default: false)")
        ->check(CLI::IsMember({"true", "false"}))->type_name("BOOL");
    const auto save_repository = [&] {
        result.config.iiif_storage = storage == "s3" ? StorageKind::s3 : StorageKind::local;
        result.config.s3.path_style = path_style == "true";
        if (!bucket_option->empty()) result.config.s3_bucket = bucket;
    };
    const auto credential_validator = CLI::Validator([](const std::string& value) {
        // Do not trim or normalize credential bytes. Reject header controls.
        for (const unsigned char byte : value)
            if (byte < 0x20 || byte == 0x7f) return std::string{"Credential contains a control byte"};
        return value.empty() ? std::string{"Credential must not be empty"} : std::string{};
    }, "CREDENTIAL");
    auto* access_option = app.add_option("--s3-access-key", result.config.s3.access_key,
        "S3 access key (SIPI_S3_ACCESS_KEY; default: SDK provider chain)")
        ->check(credential_validator)->type_name("KEY");
    auto* secret_option = app.add_option("--s3-secret-key", result.config.s3.secret_key,
        "S3 secret key (SIPI_S3_SECRET_KEY; requires access key)")
        ->check(credential_validator)->type_name("SECRET");
    auto* token_option = app.add_option("--s3-session-token", result.config.s3.session_token,
        "Optional S3 session token (SIPI_S3_SESSION_TOKEN; requires key pair)")
        ->check(credential_validator)->type_name("TOKEN");
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
        const auto cli_script = preflight::parse_script_settings(script_config, script_secrets, script_secret_files);
        preflight::validate_script_settings(cli_script);
        result.config.preflight_settings = cli_script;
        if (help || version) {
            result.config.log_level = levels.at(log_level);
            result.config.size_upscaling = size_upscaling == "true";
            result.config.bitonal_method = bitonal_method == "threshold" ? BitonalMethod::threshold : BitonalMethod::floyd_steinberg;
            result.config.jpeg.sampling = jpeg_sampling == "4:4:4" ? JpegSampling::yuv444 : JpegSampling::yuv420;
            if (!preflight_option->empty()) result.config.preflight_script = preflight_script;
            if (!root_option->empty()) result.config.iiif_root = iiif_root;
            if (!cache_dir_option->empty()) result.config.cache_dir = cache_dir;
            save_public_url();
            save_repository();
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
        // Same per-field precedence as existing settings, without reading the
        // process environment or consulting the AWS SDK inside this loader.
        const auto load_setting = [](CLI::Option* option, std::optional<std::string_view> value) {
            if (option->empty() && value) {
                option->add_result(std::string{*value});
                option->run_callback();
            }
        };
        load_setting(listen_option, env_listen_address);
        load_setting(metadata_entries_option, cache_environment.metadata_max_entries);
        load_setting(metadata_bytes_option, cache_environment.metadata_max_bytes);
        load_setting(mask_entries_option, cache_environment.mask_max_entries);
        load_setting(mask_bytes_option, cache_environment.mask_max_bytes);
        load_setting(variant_entries_option, cache_environment.variant_max_entries);
        load_setting(variant_bytes_option, cache_environment.variant_max_bytes);
        load_setting(cache_dir_option, cache_environment.directory);
        load_setting(cache_max_option, cache_environment.max_bytes);
        load_setting(cache_target_option, cache_environment.target_percent);
        load_setting(storage_option, repository_environment.storage);
        load_setting(bucket_option, repository_environment.bucket);
        load_setting(region_option, repository_environment.region);
        load_setting(endpoint_option, repository_environment.endpoint);
        load_setting(path_style_option, repository_environment.path_style);
        load_setting(access_option, s3_environment.access_key);
        load_setting(secret_option, s3_environment.secret_key);
        load_setting(token_option, s3_environment.session_token);
        load_setting(workers_option, env_iiif_workers);
        load_setting(bitonal_option, env_bitonal_method);
        load_setting(jpeg_quality_option, jpeg_environment.quality);
        load_setting(jpeg_sampling_option, jpeg_environment.sampling);
        load_setting(width_option, output_environment.width);
        load_setting(height_option, output_environment.height);
        load_setting(area_option, output_environment.area);
        load_setting(origins_option, cors_environment.origins);
        load_setting(credentials_option, cors_environment.credentials);
        // These collections merge by name, unlike scalar options. Retain the
        // CLI snapshot and let CLI11 read all file entries into fresh vectors.
        script_config_option->clear(); script_secret_option->clear(); script_file_option->clear();
        script_config.clear(); script_secrets.clear(); script_secret_files.clear();
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
        result.config.preflight_settings = preflight::merge_script_settings(cli_script,
            preflight::parse_script_environment(preflight_environment),
            preflight::parse_script_settings(script_config, script_secrets, script_secret_files));
    } catch (const preflight::ScriptConfigError& error) {
        return ParseError{error.what()}; // Only fixed, value-free messages from this module.
    } catch (const CLI::ParseError& error) {
        // CLI11 diagnostics may echo arbitrary argument/file values, including
        // secrets on duplicate, conversion or unknown-option errors. Keep only
        // the library's fixed error category, never the raw diagnostic.
        return ParseError{"Invalid startup configuration (" + error.get_name() +
                          "). Check option names, values and duplicates; use --help."};
    }
    result.config.cors_credentials = cors_credentials == "true";
    if (result.config.cors_credentials && cors_origins == "*")
        return ParseError{"cors-credentials=true requires explicit cors-origins"};
    result.config.cors_origins.clear();
    for (std::size_t begin = 0;;) {
        const auto end = cors_origins.find(',', begin);
        result.config.cors_origins.push_back(cors_origins.substr(begin, end - begin));
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    save_repository();
    if (result.config.iiif_storage == StorageKind::s3 && !result.config.s3_bucket)
        return ParseError{"iiif-storage=s3 requires s3-bucket"};
    const auto& credentials = result.config.s3;
    if (credentials.access_key.empty() != credentials.secret_key.empty())
        return ParseError{"S3 access key and secret key must both be configured"};
    if (!credentials.session_token.empty() && credentials.access_key.empty())
        return ParseError{"S3 session token requires an access key and secret key"};
    if (!preflight_option->empty()) result.config.preflight_script = preflight_script;
    if (!result.config.preflight_settings.empty() && !result.config.preflight_script)
        return ParseError{"Preflight settings require preflight-script"};
    if (!root_option->empty()) result.config.iiif_root = iiif_root;
    if (!cache_dir_option->empty()) result.config.cache_dir = cache_dir;
    save_public_url();
    result.config.log_level = levels.at(log_level);
    result.config.size_upscaling = size_upscaling == "true";
    result.config.bitonal_method = bitonal_method == "threshold" ? BitonalMethod::threshold : BitonalMethod::floyd_steinberg;
    result.config.jpeg.sampling = jpeg_sampling == "4:4:4" ? JpegSampling::yuv444 : JpegSampling::yuv420;
    return result;
}

} // namespace sipi
