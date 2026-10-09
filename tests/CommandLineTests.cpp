#include "CommandLine.h"

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

/** One end-to-end loader case; no process environment is modified by tests. */
struct Case {
    std::string name;
    std::vector<std::string> arguments;
    std::optional<std::string> environment = std::nullopt;
    bool error = false;
    sipi::Command command = sipi::Command::run;
    sipi::LogLevel level = sipi::LogLevel::info;
    std::optional<std::string> port_environment = std::nullopt;
    std::uint16_t port = 8080;
    std::optional<std::string> upscale_environment = std::nullopt;
    bool upscale = false;
    std::optional<std::string> preflight_environment = std::nullopt;
    std::optional<std::filesystem::path> preflight_script = std::nullopt;
    std::optional<std::string> root_environment = std::nullopt;
    std::optional<std::filesystem::path> iiif_root = std::nullopt;
    std::optional<std::string> url_environment = std::nullopt;
    std::optional<std::string> public_base_url = std::nullopt;
    sipi::CacheEnvironment cache_environment = {};
    std::optional<std::filesystem::path> cache_dir = std::nullopt;
    std::uint64_t cache_max_bytes = 1073741824;
    unsigned cache_target_percent = 90;
    sipi::cache::MetadataCacheLimits metadata_limits = {};
    sipi::cache::MaskCacheLimits mask_limits = {};
    sipi::cache::VariantCacheLimits variant_limits{};
    std::optional<std::string_view> workers_environment = std::nullopt;
    unsigned workers = sipi::http::default_worker_count();
    std::optional<std::string_view> bitonal_environment = std::nullopt;
    sipi::BitonalMethod bitonal_method = sipi::BitonalMethod::threshold;
    sipi::OutputLimitEnvironment output_environment = {};
    sipi::OutputLimits output_limits = {};
    sipi::CorsEnvironment cors_environment = {};
    std::vector<std::string> cors_origins{"*"};
    bool cors_credentials = false;
    std::optional<std::string_view> listen_environment = std::nullopt;
    std::string listen_address = "127.0.0.1";
    sipi::JpegEnvironment jpeg_environment;
    sipi::JpegOptions jpeg;
};

/** Check either a complete typed result or a nonempty diagnostic. */
bool check(const Case& test) {
    std::vector<std::string_view> arguments(test.arguments.begin(), test.arguments.end());
    std::optional<std::string_view> environment;
    if (test.environment) {
        environment = *test.environment;
    }
    std::optional<std::string_view> port_environment;
    if (test.port_environment) {
        port_environment = *test.port_environment;
    }
    std::optional<std::string_view> upscale_environment;
    if (test.upscale_environment) { upscale_environment = *test.upscale_environment; }
    std::optional<std::string_view> preflight_environment;
    if (test.preflight_environment) preflight_environment = *test.preflight_environment;
    std::optional<std::string_view> root_environment;
    if (test.root_environment) root_environment = *test.root_environment;
    std::optional<std::string_view> url_environment;
    if (test.url_environment) url_environment = *test.url_environment;
    const auto result = sipi::load_startup_options(arguments, environment, port_environment,
        upscale_environment, preflight_environment, root_environment, url_environment, {}, {}, test.cache_environment, test.workers_environment, test.bitonal_environment, test.output_environment, test.cors_environment, {}, test.listen_environment, test.jpeg_environment);
    if (const auto* error = std::get_if<sipi::ParseError>(&result)) {
        if (test.error && !error->message.empty()) {
            return true;
        }
        std::cerr << test.name << ": unexpected error: " << error->message << '\n';
        return false;
    }
    const auto& options = std::get<sipi::StartupOptions>(result);
    const bool matches = !test.error && options.command == test.command &&
                         options.config.listen_address == test.listen_address &&
                         options.config.jpeg.quality == test.jpeg.quality &&
                         options.config.jpeg.sampling == test.jpeg.sampling &&
                         options.config.cors_origins == test.cors_origins &&
                         options.config.cors_credentials == test.cors_credentials &&
                         options.config.output_limits.max_width == test.output_limits.max_width &&
                         options.config.output_limits.max_height == test.output_limits.max_height &&
                         options.config.output_limits.max_area == test.output_limits.max_area &&
                         options.config.iiif_workers == test.workers &&
                         options.config.bitonal_method == test.bitonal_method &&
                         options.config.log_level == test.level && options.config.port == test.port &&
                         options.config.size_upscaling == test.upscale &&
                         options.config.preflight_script == test.preflight_script &&
                         options.config.iiif_root == test.iiif_root &&
                         options.config.public_base_url == test.public_base_url &&
                         options.config.cache_dir == test.cache_dir &&
                         options.config.cache_limits.max_bytes == test.cache_max_bytes &&
                         options.config.cache_limits.target_percent == test.cache_target_percent &&
                         options.config.mask_cache_limits.max_entries == test.mask_limits.max_entries &&
                         options.config.mask_cache_limits.max_bytes == test.mask_limits.max_bytes &&
                         options.config.variant_cache_limits.max_entries == test.variant_limits.max_entries &&
                         options.config.variant_cache_limits.max_bytes == test.variant_limits.max_bytes &&
                         options.config.metadata_cache_limits.max_entries == test.metadata_limits.max_entries &&
                         options.config.metadata_cache_limits.max_bytes == test.metadata_limits.max_bytes &&
                         (options.command != sipi::Command::help ||
                          options.help_text.find("--log-level") != std::string::npos);
    if (!matches) {
        std::cerr << test.name << ": unexpected success or settings\n";
    }
    return matches;
}

} // namespace

/** Run CLI, source-priority, and file validation cases using read-only fixtures. */
int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Expected fixture directory\n";
        return 1;
    }
    const std::filesystem::path fixtures{argv[1]};
    const auto file = [&](const char* name) { return (fixtures / name).string(); };
    using sipi::Command;
    using sipi::LogLevel;
    std::vector<Case> cases{
        {"defaults", {}},
        {"help", {"--help"}, {}, false, Command::help},
        {"version", {"--version"}, {}, false, Command::version},
        {"help and level", {"--help", "--log-level", "debug"}, {}, false, Command::help, LogLevel::debug},
        {"level and version", {"--log-level", "debug", "--version"}, {}, false, Command::version, LogLevel::debug},
        {"equals syntax", {"--log-level=debug"}, {}, false, Command::run, LogLevel::debug},
        {"environment", {}, "warn", false, Command::run, LogLevel::warn},
        {"invalid environment", {}, "verbose", true},
        {"empty environment", {}, "", true},
        {"file", {"--config", file("warn.toml")}, {}, false, Command::run, LogLevel::warn},
        {"empty file", {"--config", file("empty.toml")}},
        {"environment over file", {"--config", file("warn.toml")}, "debug", false, Command::run, LogLevel::debug},
        {"CLI over environment and file", {"--config", file("warn.toml"), "--log-level", "error"}, "debug", false, Command::run, LogLevel::error},
        {"explicit default over file", {"--config", file("warn.toml"), "--log-level", "info"}},
        {"CLI overrides invalid environment", {"--log-level", "debug"}, "invalid", false, Command::run, LogLevel::debug},
        {"invalid file value", {"--config", file("invalid.toml")}, {}, true},
        {"CLI overrides invalid file value", {"--config", file("invalid.toml"), "--log-level", "info"}},
        {"environment overrides invalid file value", {"--config", file("invalid.toml")}, "debug", false, Command::run, LogLevel::debug},
        {"unknown file key", {"--config", file("unknown.toml")}, {}, true},
        {"unknown file key with CLI override", {"--config", file("unknown.toml"), "--log-level", "info"}, {}, true},
        {"action in file", {"--config", file("action.toml")}, {}, true},
        {"missing file", {"--config", file("does-not-exist.toml")}, {}, true},
        {"empty file path", {"--config", ""}, {}, true},
        {"missing file argument", {"--config"}, {}, true},
        {"repeated file option", {"--config", file("warn.toml"), "--config", file("empty.toml")}, {}, true},
        {"help skips external sources", {"--help", "--config", file("does-not-exist.toml")}, "invalid", false, Command::help},
        {"version skips external sources", {"--version"}, "invalid", false, Command::version},
    };
    const std::vector<std::pair<std::string, LogLevel>> levels{
        {"trace", LogLevel::trace}, {"debug", LogLevel::debug}, {"info", LogLevel::info},
        {"warn", LogLevel::warn}, {"error", LogLevel::error}, {"critical", LogLevel::critical}, {"off", LogLevel::off}};
    for (const auto& [name, level] : levels) {
        cases.push_back({"level " + name, {"--log-level", name}, {}, false, Command::run, level});
    }
    const std::vector<std::vector<std::string>> invalid{
        {"--log-level"}, {"--log-level", "verbose"}, {"--log-level", "DEBUG"},
        {"--log-level", ""}, {"--log-level", "--help"}, {"--unknown"}, {"image.tif"},
        {"--help", "--unknown"}, {"--help", "--version"}, {"--version", "--help"},
        {"--help", "--help"}, {"--version", "--version"},
        {"--log-level", "info", "--log-level", "debug"}, {"--run"},
        {"--log-level", "fatal"}, {"--log-level", "debug", "info"},
        {"--log-level", "debug", "--log-level"}, {"--log-level", "2"}, {"--help=false"}};
    for (const auto& arguments : invalid) {
        std::string name = "invalid:";
        for (const auto& argument : arguments) {
            name += " " + argument;
        }
        cases.push_back({name, arguments, {}, true});
    }
    cases.push_back({"port CLI", {"--port", "9090"}, {}, false, Command::run, LogLevel::info, {}, 9090});
    cases.push_back({"port env", {}, {}, false, Command::run, LogLevel::info, "9091", 9091});
    cases.push_back({"port file", {"--config", file("port.toml")}, {}, false, Command::run, LogLevel::info, {}, 9092});
    cases.push_back({"port env over file", {"--config", file("port.toml")}, {}, false, Command::run, LogLevel::info, "9091", 9091});
    cases.push_back({"port CLI over env and file", {"--config", file("port.toml"), "--port", "8080"}, {}, false, Command::run, LogLevel::info, "9091", 8080});
    cases.push_back({"port CLI over invalid env", {"--port", "9090"}, {}, false, Command::run, LogLevel::info, "invalid", 9090});
    cases.push_back({"help skips invalid port env", {"--help"}, {}, false, Command::help, LogLevel::info, "invalid"});
    for (const auto& value : {"0", "65536", "-1", "abc", "", "80.5"}) {
        cases.push_back({"invalid CLI port", {"--port", value}, {}, true});
        cases.push_back({"invalid environment port", {}, {}, true, Command::run, LogLevel::info, value});
    }
    cases.push_back({"missing port", {"--port"}, {}, true});
    cases.push_back({"duplicate port", {"--port", "8080", "--port", "9090"}, {}, true});
    cases.push_back({"invalid file port", {"--config", file("invalid-port.toml")}, {}, true});
    // Named assignments keep the new boolean's precedence cases easy to read.
    const auto upscale_case = [&](std::string name, std::vector<std::string> args,
                                  std::optional<std::string> env, bool expected, bool error = false) {
        Case test;
        test.name = std::move(name);
        test.arguments = std::move(args);
        test.upscale_environment = std::move(env);
        test.upscale = expected;
        test.error = error;
        cases.push_back(std::move(test));
    };
    upscale_case("upscale CLI", {"--size-upscaling", "true"}, {}, true);
    upscale_case("upscale environment", {}, "true", true);
    upscale_case("upscale file", {"--config", file("upscale.toml")}, {}, true);
    upscale_case("upscale CLI false beats true", {"--config", file("upscale.toml"), "--size-upscaling=false"}, "true", false);
    upscale_case("upscale environment false beats file", {"--config", file("upscale.toml")}, "false", false);
    upscale_case("upscale overridden invalid env", {"--size-upscaling", "false"}, "bad", false);
    upscale_case("upscale overridden invalid file", {"--config", file("invalid-upscale.toml")}, "true", true);
    upscale_case("upscale invalid file", {"--config", file("invalid-upscale.toml")}, {}, false, true);
    upscale_case("upscale missing value", {"--size-upscaling"}, {}, false, true);
    upscale_case("upscale duplicate", {"--size-upscaling=true", "--size-upscaling=false"}, {}, false, true);
    for (const auto* invalid_bool : {"", "1", "yes", "TRUE", "False", "invalid"}) {
        upscale_case("invalid upscale CLI", {"--size-upscaling", invalid_bool}, {}, false, true);
        upscale_case("invalid upscale env", {}, invalid_bool, false, true);
    }
    Case help_upscale;
    help_upscale.name = "help skips invalid upscale environment";
    help_upscale.arguments = {"--help"};
    help_upscale.command = Command::help;
    help_upscale.upscale_environment = "bad";
    cases.push_back(help_upscale);
    const auto preflight_case = [&](std::string name, std::vector<std::string> args,
                                    std::optional<std::string> env,
                                    std::optional<std::filesystem::path> expected,
                                    bool error = false, Command command = Command::run) {
        Case test;
        test.name = std::move(name);
        test.arguments = std::move(args);
        test.preflight_environment = std::move(env);
        test.preflight_script = std::move(expected);
        test.error = error;
        test.command = command;
        cases.push_back(std::move(test));
    };
    preflight_case("script CLI", {"--preflight-script", "not yet present.lua"}, {}, "not yet present.lua");
    preflight_case("script env", {}, "/etc/sipi/policy.lua", "/etc/sipi/policy.lua");
    preflight_case("script file", {"--config", file("preflight.toml")}, {}, "scripts/policy with spaces.lua");
    preflight_case("script env over file", {"--config", file("preflight.toml")}, "env.lua", "env.lua");
    preflight_case("script CLI over env/file", {"--config", file("preflight.toml"), "--preflight-script=cli.lua"}, "env.lua", "cli.lua");
    preflight_case("script CLI over empty env", {"--preflight-script=cli.lua"}, "", "cli.lua");
    preflight_case("script env over empty file", {"--config", file("empty-preflight.toml")}, "env.lua", "env.lua");
    preflight_case("script empty file", {"--config", file("empty-preflight.toml")}, {}, {}, true);
    preflight_case("script empty env", {}, "", {}, true);
    preflight_case("script empty CLI", {"--preflight-script", ""}, {}, {}, true);
    preflight_case("script missing value", {"--preflight-script"}, {}, {}, true);
    preflight_case("script duplicate", {"--preflight-script=a.lua", "--preflight-script=b.lua"}, {}, {}, true);
    preflight_case("script NUL CLI", {"--preflight-script", std::string("a\0b", 3)}, {}, {}, true);
    preflight_case("script NUL env", {}, std::string("a\0b", 3), {}, true);
    preflight_case("script help ignores env", {"--help"}, "", {}, false, Command::help);
    preflight_case("script version ignores env", {"--version"}, "", {}, false, Command::version);
    preflight_case("script help validates CLI", {"--help", "--preflight-script", ""}, {}, {}, true);
    preflight_case("script help does not open script", {"--help", "--preflight-script=missing.lua"}, {}, "missing.lua", false, Command::help);
    const auto root_case = [&](std::string name, std::vector<std::string> args,
                               std::optional<std::string> env,
                               std::optional<std::filesystem::path> expected,
                               bool error = false, Command command = Command::run) {
        Case test;
        test.name = std::move(name);
        test.arguments = std::move(args);
        test.root_environment = std::move(env);
        test.iiif_root = std::move(expected);
        test.error = error;
        test.command = command;
        cases.push_back(std::move(test));
    };
    root_case("root CLI", {"--iiif-root", "data/images"}, {}, "data/images");
    root_case("root env", {}, "/srv/images", "/srv/images");
    root_case("root file", {"--config", file("root.toml")}, {}, "images with spaces");
    root_case("root env over file", {"--config", file("root.toml")}, "env", "env");
    root_case("root CLI over env/file", {"--config", file("root.toml"), "--iiif-root=cli"}, "env", "cli");
    root_case("root CLI overrides empty env", {"--iiif-root=cli"}, "", "cli");
    root_case("root env overrides invalid file", {"--config", file("empty-root.toml")}, "env", "env");
    root_case("empty root file", {"--config", file("empty-root.toml")}, {}, {}, true);
    root_case("empty root env", {}, "", {}, true);
    root_case("empty root CLI", {"--iiif-root", ""}, {}, {}, true);
    root_case("missing root", {"--iiif-root"}, {}, {}, true);
    root_case("duplicate root", {"--iiif-root=a", "--iiif-root=b"}, {}, {}, true);
    root_case("NUL root CLI", {"--iiif-root", std::string("a\0b", 3)}, {}, {}, true);
    root_case("NUL root env", {}, std::string("a\0b", 3), {}, true);
    root_case("root help skips env", {"--help"}, "", {}, false, Command::help);
    root_case("root version skips env", {"--version"}, "", {}, false, Command::version);
    root_case("root help validates CLI", {"--help", "--iiif-root", ""}, {}, {}, true);
    root_case("root help skips filesystem", {"--help", "--iiif-root=missing"}, {}, "missing", false, Command::help);
    const auto url_case = [&](std::string name, std::vector<std::string> args,
                              std::optional<std::string> env, std::optional<std::string> expected,
                              bool error = false, Command command = Command::run) {
        Case test;
        test.name = std::move(name);
        test.arguments = std::move(args);
        test.url_environment = std::move(env);
        test.public_base_url = std::move(expected);
        test.error = error;
        test.command = command;
        cases.push_back(std::move(test));
    };
    url_case("URL CLI", {"--public-base-url=https://images.example.org/"}, {}, "https://images.example.org");
    url_case("URL IPv6", {"--public-base-url=http://[::1]:9090/iiif"}, {}, "http://[::1]:9090/iiif");
    url_case("URL env", {}, "https://images.example.org/museum%20images/", "https://images.example.org/museum%20images");
    url_case("URL file", {"--config", file("public-url.toml")}, {}, "https://images.example.org/iiif");
    url_case("URL env over file", {"--config", file("public-url.toml")}, "https://env.test", "https://env.test");
    url_case("URL CLI over env/file", {"--config", file("public-url.toml"), "--public-base-url=http://cli.test"}, "https://env.test", "http://cli.test");
    url_case("URL CLI over invalid env", {"--public-base-url=http://localhost:8080"}, "bad", "http://localhost:8080");
    url_case("URL invalid file", {"--config", file("invalid-public-url.toml")}, {}, {}, true);
    url_case("URL env over invalid file", {"--config", file("invalid-public-url.toml")}, "https://env.test", "https://env.test");
    url_case("URL duplicate", {"--public-base-url=http://a.test", "--public-base-url=http://b.test"}, {}, {}, true);
    url_case("URL missing value", {"--public-base-url"}, {}, {}, true);
    url_case("URL help skips env", {"--help"}, "bad", {}, false, Command::help);
    url_case("URL help validates CLI", {"--help", "--public-base-url=bad"}, {}, {}, true);
    for (const auto& value : {"", "example.org", "ftp://example.org", "https:///x", "https://user:pass@example.org",
                             "https://example.org?q=1", "https://example.org/#fragment", "https://example.org/a b",
                             "https://example.org:0", "https://example.org:65536", "https://example.org:999999999999999999999",
                             "http://[::::]", "https://a..test", "https://-a.test", "https://example.org//",
                             "https://example.org/a/../b", "https://example.org/%2e", "https://example.org/%GG",
                             "https://example.org/%2", "https://example.org/%00", "https://example.org/\\bad"}) {
        url_case("invalid URL CLI", {"--public-base-url", value}, {}, {}, true);
        url_case("invalid URL env", {}, value, {}, true);
    }
    const auto cache_case = [&](std::string name, std::vector<std::string> args,
                                sipi::CacheEnvironment env,
                                std::optional<std::filesystem::path> directory,
                                std::uint64_t limit = 1073741824, unsigned target = 90,
                                bool error = false, Command command = Command::run) {
        Case test;
        test.name = std::move(name); test.arguments = std::move(args);
        test.cache_environment = env; test.cache_dir = std::move(directory);
        test.cache_max_bytes = limit; test.cache_target_percent = target;
        test.error = error; test.command = command;
        cases.push_back(std::move(test));
    };
    cache_case("cache file", {"--config",file("cache.toml")}, {}, "cache with spaces", 1000,80);
    cache_case("cache env over file", {"--config",file("cache.toml")}, {"env","2000","85"}, "env",2000,85);
    cache_case("cache CLI over env/file", {"--config",file("cache.toml"),"--cache-dir=cli","--cache-max-bytes=3000","--cache-target-percent=75"},
        {"env","2000","85"}, "cli",3000,75);
    cache_case("cache mixed priority", {"--config",file("cache.toml"),"--cache-dir=cli"}, {{},"2000",{}}, "cli",2000,80);
    cache_case("cache CLI ignores invalid env", {"--cache-dir=cli","--cache-max-bytes=3000","--cache-target-percent=75"},
        {"","bad","0"}, "cli",3000,75);
    cache_case("cache help skips env", {"--help"}, {"","bad","0"}, {},1073741824,90,false,Command::help);
    for (const auto& args : std::vector<std::vector<std::string>>{
        {"--cache-dir",""},{"--cache-dir"},{"--cache-dir=a","--cache-dir=b"},
        {"--cache-dir",std::string("a\0b",3)},{"--cache-max-bytes=0"},
        {"--cache-max-bytes=-1"},{"--cache-max-bytes=1.5"},{"--cache-max-bytes=4611686018427387904"},
        {"--cache-max-bytes=18446744073709551616"},{"--cache-target-percent=0"},
        {"--cache-target-percent=100"},{"--cache-target-percent=bad"},
        {"--help","--cache-max-bytes=0"},{"--config",file("invalid-cache.toml")}})
        cache_case("invalid cache CLI/file",args,{}, {},1073741824,90,true);
    for (const auto& env : std::vector<sipi::CacheEnvironment>{{"",{},{}},{{},"0",{}},{{},{},"100"}})
        cache_case("invalid cache environment",{},env,{},1073741824,90,true);
    const auto metadata_case = [&](std::vector<std::string> args,
                                   std::optional<std::string_view> entries,
                                   std::optional<std::string_view> bytes,
                                   sipi::cache::MetadataCacheLimits expected,
                                   bool error = false, Command command = Command::run) {
        Case test;
        test.name = "metadata cache configuration";
        test.arguments = std::move(args);
        test.cache_environment.metadata_max_entries = entries;
        test.cache_environment.metadata_max_bytes = bytes;
        test.metadata_limits = expected;
        test.error = error; test.command = command;
        cases.push_back(std::move(test));
    };
    metadata_case({"--config",file("metadata-cache.toml")}, {}, {}, {12,4096});
    metadata_case({"--config",file("metadata-cache.toml")}, "20", "8192", {20,8192});
    metadata_case({"--config",file("metadata-cache.toml"),"--metadata-cache-max-entries=0"}, "20", "8192", {0,8192});
    metadata_case({"--config",file("metadata-cache.toml"),"--metadata-cache-max-entries=256","--metadata-cache-max-bytes=67108864"},
                  "0", "0", {});
    metadata_case({}, "0", "0", {0,0});
    metadata_case({"--metadata-cache-max-bytes=0"}, {}, {}, {256,0});
    metadata_case({"--metadata-cache-max-entries=1","--metadata-cache-max-bytes=2"}, "bad", "bad", {1,2});
    metadata_case({"--config",file("invalid-metadata-cache.toml")}, "1", "2", {1,2});
    metadata_case({"--config",file("invalid-metadata-cache.toml")}, {}, {}, {}, true);
    metadata_case({"--help"}, "bad", "bad", {}, false, Command::help);
    metadata_case({"--version"}, "bad", "bad", {}, false, Command::version);
    for (const char* invalid_value : {"", "-1", "+1", "1.5", "1MB", "18446744073709551616"}) {
        metadata_case({"--metadata-cache-max-entries",invalid_value}, {}, {}, {}, true);
        metadata_case({"--metadata-cache-max-bytes",invalid_value}, {}, {}, {}, true);
        metadata_case({}, invalid_value, {}, {}, true);
        metadata_case({}, {}, invalid_value, {}, true);
    }
    metadata_case({"--metadata-cache-max-entries"}, {}, {}, {}, true);
    metadata_case({"--metadata-cache-max-bytes=1","--metadata-cache-max-bytes=2"}, {}, {}, {}, true);
    metadata_case({"--help","--metadata-cache-max-entries=-1"}, {}, {}, {}, true);
    const auto mask_case = [&](std::vector<std::string> args,
                                   std::optional<std::string_view> entries,
                                   std::optional<std::string_view> bytes,
                                   sipi::cache::MaskCacheLimits expected,
                                   bool error = false, Command command = Command::run) {
        Case test;
        test.name = "mask cache configuration";
        test.arguments = std::move(args);
        test.cache_environment.mask_max_entries = entries;
        test.cache_environment.mask_max_bytes = bytes;
        test.mask_limits = expected;
        test.error = error; test.command = command;
        cases.push_back(std::move(test));
    };
    mask_case({"--config",file("mask-cache.toml")}, {}, {}, {12,4096});
    mask_case({"--config",file("mask-cache.toml")}, "20", "8192", {20,8192});
    mask_case({"--config",file("mask-cache.toml"),"--mask-cache-max-entries=0"}, "20", "8192", {0,8192});
    mask_case({"--config",file("mask-cache.toml"),"--mask-cache-max-entries=64","--mask-cache-max-bytes=33554432"},
                  "0", "0", {});
    mask_case({}, "0", "0", {0,0});
    mask_case({"--mask-cache-max-bytes=0"}, {}, {}, {64,0});
    mask_case({"--mask-cache-max-entries=1","--mask-cache-max-bytes=2"}, "bad", "bad", {1,2});
    mask_case({"--config",file("invalid-mask-cache.toml")}, "1", "2", {1,2});
    mask_case({"--config",file("invalid-mask-cache.toml")}, {}, {}, {}, true);
    mask_case({"--help"}, "bad", "bad", {}, false, Command::help);
    mask_case({"--version"}, "bad", "bad", {}, false, Command::version);
    for (const char* invalid_value : {"", "-1", "+1", "1.5", "1MB", "18446744073709551616"}) {
        mask_case({"--mask-cache-max-entries",invalid_value}, {}, {}, {}, true);
        mask_case({"--mask-cache-max-bytes",invalid_value}, {}, {}, {}, true);
        mask_case({}, invalid_value, {}, {}, true);
        mask_case({}, {}, invalid_value, {}, true);
    }
    mask_case({"--mask-cache-max-entries"}, {}, {}, {}, true);
    mask_case({"--mask-cache-max-bytes=1","--mask-cache-max-bytes=2"}, {}, {}, {}, true);
    mask_case({"--help","--mask-cache-max-entries=-1"}, {}, {}, {}, true);
    const auto variant_case = [&](std::vector<std::string> args,
                                   std::optional<std::string_view> entries,
                                   std::optional<std::string_view> bytes,
                                   sipi::cache::VariantCacheLimits expected,
                                   bool error = false, Command command = Command::run) {
        Case test;
        test.name = "variant cache configuration";
        test.arguments = std::move(args);
        test.cache_environment.variant_max_entries = entries;
        test.cache_environment.variant_max_bytes = bytes;
        test.variant_limits = expected;
        test.error = error; test.command = command;
        cases.push_back(std::move(test));
    };
    variant_case({"--config",file("variant-cache.toml")}, {}, {}, {12,4096});
    variant_case({"--config",file("variant-cache.toml")}, "20", "8192", {20,8192});
    variant_case({"--config",file("variant-cache.toml"),"--variant-cache-max-entries=0"}, "20", "8192", {0,8192});
    variant_case({"--config",file("variant-cache.toml"),"--variant-cache-max-entries=16","--variant-cache-max-bytes=67108864"},
                  "0", "0", {});
    variant_case({}, "0", "0", {0,0});
    variant_case({"--variant-cache-max-bytes=0"}, {}, {}, {16,0});
    variant_case({"--variant-cache-max-entries=1","--variant-cache-max-bytes=2"}, "bad", "bad", {1,2});
    variant_case({"--config",file("invalid-variant-cache.toml")}, "1", "2", {1,2});
    variant_case({"--config",file("invalid-variant-cache.toml")}, {}, {}, {}, true);
    variant_case({"--help"}, "bad", "bad", {}, false, Command::help);
    variant_case({"--version"}, "bad", "bad", {}, false, Command::version);
    for (const char* invalid_value : {"", "-1", "+1", "1.5", "1MB", "18446744073709551616"}) {
        variant_case({"--variant-cache-max-entries",invalid_value}, {}, {}, {}, true);
        variant_case({"--variant-cache-max-bytes",invalid_value}, {}, {}, {}, true);
        variant_case({}, invalid_value, {}, {}, true);
        variant_case({}, {}, invalid_value, {}, true);
    }
    variant_case({"--variant-cache-max-entries"}, {}, {}, {}, true);
    variant_case({"--variant-cache-max-bytes=1","--variant-cache-max-bytes=2"}, {}, {}, {}, true);
    variant_case({"--help","--variant-cache-max-entries=-1"}, {}, {}, {}, true);
    const auto worker_case = [&](std::vector<std::string> args,
                                  std::optional<std::string_view> env, unsigned expected,
                                  bool error = false, Command command = Command::run) {
        Case test{"worker configuration", std::move(args)};
        test.workers_environment = env; test.workers = expected;
        test.error = error; test.command = command; cases.push_back(std::move(test));
    };
    worker_case({"--config",file("workers.toml")}, {}, 2);
    worker_case({"--config",file("workers.toml")}, "3", 3);
    worker_case({"--config",file("workers.toml"),"--iiif-workers=1"}, "3", 1);
    worker_case({"--iiif-workers=32"}, "bad", 32);
    worker_case({"--help"}, "bad", sipi::http::default_worker_count(), false, Command::help);
    for (const char* invalid : {"", "0", "33", "-1", "+2", "1.5", "999999999999999999999"}) {
        worker_case({"--iiif-workers",invalid}, {}, 0, true);
        worker_case({}, invalid, 0, true);
    }
    worker_case({"--iiif-workers=1","--iiif-workers=2"}, {}, 0, true);
    worker_case({"--help","--iiif-workers=0"}, {}, 0, true);
    const auto bitonal_case = [&](std::vector<std::string> args,
                                  std::optional<std::string_view> env,
                                  sipi::BitonalMethod expected, bool error = false,
                                  Command command = Command::run) {
        Case test{"bitonal configuration", std::move(args)};
        test.bitonal_environment = env; test.bitonal_method = expected;
        test.error = error; test.command = command; cases.push_back(std::move(test));
    };
    using enum sipi::BitonalMethod;
    bitonal_case({}, {}, threshold);
    bitonal_case({"--config",file("bitonal.toml")}, {}, floyd_steinberg);
    bitonal_case({"--config",file("bitonal.toml")}, "threshold", threshold);
    bitonal_case({"--config",file("bitonal.toml"),"--bitonal-method=floyd-steinberg"}, "threshold", floyd_steinberg);
    bitonal_case({"--bitonal-method=threshold"}, "invalid", threshold);
    bitonal_case({"--help"}, "invalid", threshold, false, Command::help);
    bitonal_case({"--version"}, "invalid", threshold, false, Command::version);
    for (const char* invalid : {"", "floyd", "FLOYD-STEINBERG", "ordered"}) {
        bitonal_case({"--bitonal-method",invalid}, {}, threshold, true);
        bitonal_case({}, invalid, threshold, true);
    }
    bitonal_case({"--config",file("invalid-bitonal.toml")}, {}, threshold, true);
    bitonal_case({"--bitonal-method=threshold","--bitonal-method=floyd-steinberg"}, {}, threshold, true);
    bitonal_case({"--help","--bitonal-method=bad"}, {}, threshold, true);
    const auto output_case = [&](std::vector<std::string> args,
                                  sipi::OutputLimitEnvironment env, sipi::OutputLimits expected,
                                  bool error=false, Command command=Command::run) {
        Case test{"output limit configuration",std::move(args)};
        test.output_environment=env; test.output_limits=expected;
        test.error=error; test.command=command; cases.push_back(std::move(test));
    };
    output_case({"--config",file("output-limits.toml")},{},{1000,800,500000});
    output_case({"--config",file("output-limits.toml")},{"2000","900","600000"},{2000,900,600000});
    output_case({"--config",file("output-limits.toml"),"--max-width=3000"},
                {"2000","900","600000"},{3000,900,600000});
    output_case({"--max-width=10"},{"bad",{},{}},{10,0,8388608});
    output_case({"--help"},{"bad","bad","bad"},{},false,Command::help);
    output_case({"--version"},{"bad","bad","bad"},{},false,Command::version);
    for (const char* invalid : {"0","-1","+1","1.5","","999999999999999999999"}) {
        output_case({"--max-width",invalid},{},{},true);
        output_case({"--max-height",invalid},{},{},true);
        output_case({"--max-area",invalid},{},{},true);
        output_case({}, {invalid,{},{}},{},true);
        output_case({}, {{},invalid,{}},{},true);
        output_case({}, {{},{},invalid},{},true);
    }
    output_case({"--max-width=32769"},{},{},true);
    output_case({"--max-height=32769"},{},{},true);
    output_case({"--max-area=8388609"},{},{},true);
    output_case({"--max-width=1","--max-width=2"},{},{},true);
    output_case({"--config",file("invalid-output-limits.toml")},{},{},true);
    output_case({"--help","--max-area=0"},{},{},true);
    const auto cors_case = [&](std::vector<std::string> args, sipi::CorsEnvironment env,
                               std::vector<std::string> origins, bool credentials, bool error=false) {
        Case test{"CORS configuration", std::move(args)};
        test.cors_environment=env; test.cors_origins=std::move(origins);
        test.cors_credentials=credentials; test.error=error; cases.push_back(std::move(test));
    };
    cors_case({"--config",file("cors.toml")},{},{"https://viewer.example","https://other.example"},true);
    cors_case({"--config",file("cors.toml")},{"https://env.example","false"},{"https://env.example"},false);
    cors_case({"--config",file("cors.toml"),"--cors-origins=*","--cors-credentials=false"},
              {"https://env.example","true"},{"*"},false);
    cors_case({"--cors-credentials=true"},{},{},false,true);
    cors_case({}, {"*","true"},{},false,true);
    for (const char* invalid : {"", "null", "https://*.example", "https://host/path",
            "https://host/", "https://user@host", "https://host,", "*,https://host",
            "https://host?x", "https://host, https://other", "https://host:0"}) {
        cors_case({"--cors-origins",invalid},{},{},false,true);
        cors_case({}, {invalid,{}},{},false,true);
    }
    cors_case({"--cors-origins=http://[::1]:8080"},{},{"http://[::1]:8080"},false);
    cors_case({"--cors-credentials=yes"},{},{},false,true);
    const auto listen_case = [&](std::vector<std::string> args,
            std::optional<std::string_view> env, std::string expected, bool error=false) {
        Case test{"Listen address", std::move(args)};
        test.listen_environment=env; test.listen_address=std::move(expected);
        test.error=error; cases.push_back(std::move(test));
    };
    listen_case({"--config",file("listen.toml")},{},"0.0.0.0");
    listen_case({"--config",file("listen.toml")},"::1","::1");
    listen_case({"--config",file("listen.toml"),"--listen-address=127.0.0.2"},"invalid","127.0.0.2");
    listen_case({"--help"},"invalid","127.0.0.1");
    cases.back().command=Command::help;
    for (const char* bad : {"", "localhost", "[::1]", "1.2.3.999", "0.0.0.0:8182"}) {
        listen_case({"--listen-address",bad},{},"",true);
        listen_case({},bad,"",true);
    }
    const auto jpeg_case = [&](std::vector<std::string> args, sipi::JpegEnvironment env,
                               sipi::JpegOptions expected, bool error=false) {
        Case test{"JPEG configuration", std::move(args)};
        test.jpeg_environment = env; test.jpeg = expected; test.error = error;
        cases.push_back(std::move(test));
    };
    using enum sipi::JpegSampling;
    jpeg_case({}, {}, {90, yuv444});
    jpeg_case({"--config",file("jpeg.toml")}, {}, {75, yuv420});
    jpeg_case({"--config",file("jpeg.toml")}, {"85", "4:4:4"}, {85, yuv444});
    jpeg_case({"--config",file("jpeg.toml"),"--jpeg-quality=100"}, {"bad", "4:4:4"}, {100, yuv444});
    jpeg_case({"--jpeg-sampling=4:2:0"}, {"1", "invalid"}, {1, yuv420});
    jpeg_case({"--config",file("jpeg.toml"),"--jpeg-quality=90","--jpeg-sampling=4:4:4"}, {}, {});
    for (const char* bad : {"", "0", "101", "-1", "+90", "90.5", "high"}) {
        jpeg_case({"--jpeg-quality",bad}, {}, {}, true);
        jpeg_case({}, {bad, {}}, {}, true);
    }
    for (const char* bad : {"", "444", "4:2:2", "4:1:1"}) {
        jpeg_case({"--jpeg-sampling",bad}, {}, {}, true);
        jpeg_case({}, {{}, bad}, {}, true);
    }
    jpeg_case({"--config",file("invalid-jpeg.toml")}, {}, {}, true);
    jpeg_case({"--jpeg-quality=80","--jpeg-quality=90"}, {}, {}, true);
    jpeg_case({"--jpeg-sampling=4:4:4","--jpeg-sampling=4:2:0"}, {}, {}, true);
    jpeg_case({"--help","--jpeg-sampling=bad"}, {}, {}, true);
    for (const auto command : {Command::help, Command::version}) {
        jpeg_case({command == Command::help ? "--help" : "--version"}, {"bad", "bad"}, {});
        cases.back().command = command;
    }
    int failures = 0;
    for (const auto& test : cases) {
        failures += !check(test);
    }
    std::cout << cases.size() - failures << '/' << cases.size() << " cases passed\n";
    return failures == 0 ? 0 : 1;
}
