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
        upscale_environment, preflight_environment, root_environment, url_environment, {}, {}, test.cache_environment);
    if (const auto* error = std::get_if<sipi::ParseError>(&result)) {
        if (test.error && !error->message.empty()) {
            return true;
        }
        std::cerr << test.name << ": unexpected error: " << error->message << '\n';
        return false;
    }
    const auto& options = std::get<sipi::StartupOptions>(result);
    const bool matches = !test.error && options.command == test.command &&
                         options.config.log_level == test.level && options.config.port == test.port &&
                         options.config.size_upscaling == test.upscale &&
                         options.config.preflight_script == test.preflight_script &&
                         options.config.iiif_root == test.iiif_root &&
                         options.config.public_base_url == test.public_base_url &&
                         options.config.cache_dir == test.cache_dir &&
                         options.config.cache_limits.max_bytes == test.cache_max_bytes &&
                         options.config.cache_limits.target_percent == test.cache_target_percent &&
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
    int failures = 0;
    for (const auto& test : cases) {
        failures += !check(test);
    }
    std::cout << cases.size() - failures << '/' << cases.size() << " cases passed\n";
    return failures == 0 ? 0 : 1;
}
