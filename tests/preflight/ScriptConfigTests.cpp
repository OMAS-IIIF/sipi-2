#include "CommandLine.h"
#include "preflight/LuaPreflight.h"

#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <sys/stat.h>

namespace {
using namespace sipi;
using namespace sipi::preflight;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void write(const std::filesystem::path& path, const std::string& value) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << value;
    if (!out) throw std::runtime_error("Cannot write fixture");
}
ParseResult load(const std::vector<std::string>& args, const std::vector<std::string_view>& env = {}) {
    const std::vector<std::string_view> views(args.begin(), args.end());
    return load_startup_options(views, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, env);
}
StartupOptions success(ParseResult value) {
    if (const auto* error = std::get_if<ParseError>(&value)) throw std::runtime_error(error->message);
    return std::get<StartupOptions>(std::move(value));
}
void failure(ParseResult value) {
    const auto* error = std::get_if<ParseError>(&value);
    check(error && !error->message.empty() && error->message.find("sensitive") == std::string::npos,
          "Expected fixed redacted configuration error");
}
template<class Function> void rejects(Function fn) {
    bool failed = false;
    try { fn(); } catch (const ScriptConfigError& error) {
        failed = true;
        check(std::string(error.what()).find("sensitive") == std::string::npos, "Value-free secret file error");
    }
    check(failed, "Expected ScriptConfigError");
}
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try {
        const std::filesystem::path dir(argv[1]);
        std::filesystem::create_directories(dir);
        const auto file = dir / "settings.toml";
        write(file, R"(
preflight-script = "policy.lua"
preflight-config = ["a=file", "b=file", "c=file", "empty="]
preflight-secret = ["key=file", "file_only=file-secret"]
preflight-secret-file = ["selected=missing-sensitive-file"]
)");
        auto result = success(load({"--config", file.string(), "--preflight-config", "a=cli",
            "--preflight-config", "extra=one=two", "--preflight-secret", "key=cli-secret",
            "--preflight-secret", "selected=cli-wins"}, {
                "SIPI_PREFLIGHT_CONFIG_A=env", "SIPI_PREFLIGHT_CONFIG_B=env",
                "SIPI_PREFLIGHT_SECRETS_KEY=env", "SIPI_PREFLIGHT_SECRETS_EXTRA=env-secret",
                "SIPI_PREFLIGHT_SECRETS_SELECTED=env-wins", "UNRELATED=sensitive"}));
        const auto values = resolve_script_config(result.config.preflight_settings);
        check(values.config.at("a") == "cli" && values.config.at("b") == "env" && values.config.at("c") == "file" &&
              values.config.at("extra") == "one=two" && values.config.at("empty").empty(), "Per-key configuration precedence");
        check(values.secrets.at("key") == "cli-secret" && values.secrets.at("file_only") == "file-secret" &&
              values.secrets.at("extra") == "env-secret" && values.secrets.at("selected") == "cli-wins", "Per-key secret precedence");
        check(success(load({})).config.preflight_settings.empty(), "Default empty maps");
        failure(load({"--preflight-config", "a=sensitive"}));
        for (const auto* assignment : {"a=sensitive", "a="}) {
            auto r = success(load({"--help", "--preflight-config", assignment}));
            check(r.help_text.find("sensitive") == std::string::npos, "Help never prints values");
        }
        success(load({"--help", "--config", "missing-sensitive-file", "--preflight-secret-file", "key=missing-sensitive-file"},
                     {"SIPI_PREFLIGHT_SECRETS_KEY="}));
        for (const auto* assignment : {"UPPER=sensitive", "bad-name=sensitive", "1bad=sensitive", "=sensitive", "sensitive"})
            failure(load({"--help", "--preflight-config", assignment}));
        failure(load({"--help", "--preflight-secret", "a="}));
        failure(load({"--help", "--preflight-config", "a=sensitive", "--preflight-config", "a=sensitive"}));
        failure(load({"--help", "--preflight-secret", "a=sensitive", "--preflight-secret-file", "a=sensitive"}));
        const std::vector<std::string> script{"--preflight-script", "unused.lua"};
        failure(load(script, {"SIPI_PREFLIGHT_SECRETS_KEY="}));
        failure(load(script, {"SIPI_PREFLIGHT_SECRETS_KEY=sensitive", "SIPI_PREFLIGHT_SECRET_FILES_KEY=sensitive"}));
        failure(load(script, {"SIPI_PREFLIGHT_CONFIG_FOO=sensitive", "SIPI_PREFLIGHT_CONFIG_foo=sensitive"}));
        success(load({"--preflight-script", "unused.lua", "--preflight-secret", "key=winner"},
                     {"SIPI_PREFLIGHT_SECRETS_KEY="})); // Overridden invalid value is ignored.
        failure(load({"--help", "--preflight-secret", "key=" + std::string(16385, 's')}));
        failure(load({"--help", "--preflight-config", std::string("a=x\0sensitive", 13)}));
        std::vector<std::string> too_many{"--help"};
        for (int i=0; i<65; ++i) { too_many.push_back("--preflight-config"); too_many.push_back("key"+std::to_string(i)+"=sensitive"); }
        failure(load(too_many));
        std::vector<std::string> too_big{"--help"};
        for (int i=0; i<17; ++i) { too_big.push_back("--preflight-config"); too_big.push_back("key"+std::to_string(i)+"="+std::string(16384,'s')); }
        failure(load(too_big));
        write(file, "preflight-script='unused.lua'\npreflight-secret=['key=']\n");
        success(load({"--config",file.string()}, {"SIPI_PREFLIGHT_SECRETS_KEY=winner"}));
        failure(load({"--config",file.string()}));
        write(file, "preflight-script='unused.lua'\npreflight-secret=['key=sensitive','key=sensitive']\n");
        failure(load({"--config",file.string()}));

        const auto secret = dir / "secret.bin";
        const std::string binary("A\0B\n", 4);
        write(secret, binary);
        const auto selected = parse_script_settings({"endpoint=https://example.org"}, {}, {"key="+secret.string()});
        const auto snapshot = resolve_script_config(selected);
        check(snapshot.secrets.at("key") == binary, "Secret files retain NUL/newline bytes");
        const auto policy_path = dir / "policy.lua";
        write(policy_path, R"lua(
            assert(sipi.config.endpoint == 'https://example.org')
            assert(sipi.config.unrelated == nil and os == nil)
            assert(sipi.secrets.key == 'A'..string.char(0)..'B\n')
            sipi.config.local_value = 'initialized'
            function preflight()
                assert(sipi.config.local_value == 'initialized')
                assert(sipi.secrets.key == 'A'..string.char(0)..'B\n')
                sipi.config.endpoint = 'mutated'
                sipi.secrets.key = 'mutated'
                return {decision='allow'}
            end
        )lua");
        const LuaPreflight policy(policy_path, snapshot);
        write(secret, "changed-after-snapshot");
        policy.validate();
        std::vector<std::future<Decision>> futures;
        for (int i=0; i<4; ++i) futures.push_back(std::async(std::launch::async, [&] { return policy.evaluate({{{},"id"},{},{}}); }));
        for (auto& future : futures) check(std::holds_alternative<Allow>(future.get()), "Independent script configuration snapshots");
        write(secret, ""); rejects([&] { resolve_script_config(selected); });
        write(secret, std::string(16385, 's')); rejects([&] { resolve_script_config(selected); });
        std::filesystem::remove(secret);
        rejects([&] { resolve_script_config(selected); });
        std::filesystem::create_directory(secret);
        rejects([&] { resolve_script_config(selected); });
        std::filesystem::remove(secret);
        check(::mkfifo(secret.c_str(), 0600) == 0, "Create FIFO fixture");
        rejects([&] { resolve_script_config(selected); }); // Must not block.
        std::filesystem::remove(secret);
        std::cout << "Script configuration precedence, redaction, file boundaries and Lua isolation passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
