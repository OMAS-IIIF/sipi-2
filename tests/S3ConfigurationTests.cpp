#include "CommandLine.h"
#include <filesystem>
#include <iostream>
#include <vector>

namespace {
using Environment = sipi::S3CredentialEnvironment;
sipi::ParseResult load(const std::vector<std::string>& args, Environment env = {}, sipi::RepositoryEnvironment repository = {}) {
    const std::vector<std::string_view> views(args.begin(), args.end());
    return sipi::load_startup_options(views, {}, {}, {}, {}, {}, {}, env, repository);
}
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void valid(const std::vector<std::string>& args, Environment env,
           const std::string& access, const std::string& secret, const std::string& token = "") {
    const auto result = load(args, env);
    const auto* options = std::get_if<sipi::StartupOptions>(&result);
    check(options != nullptr, "Expected valid S3 configuration");
    check(options->config.s3.access_key == access && options->config.s3.secret_key == secret &&
          options->config.s3.session_token == token, "S3 credential precedence mismatch");
}
void invalid(const std::vector<std::string>& args, Environment env = {}) {
    const auto result = load(args, env);
    const auto* error = std::get_if<sipi::ParseError>(&result);
    check(error && !error->message.empty(), "Expected configuration error");
    check(error->message.find("do-not-disclose-this-value") == std::string::npos, "Secret leaked into diagnostic");
}
} // namespace

/** Precedence, selected-value validation, cross-field validation and redaction.
 * No AWS calls or changes to the process environment. All values are test data. */
int main(int argc, char** argv) {
    try {
        check(argc == 2, "Expected fixture directory");
        const auto file = [&](const char* name) { return (std::filesystem::path(argv[1]) / name).string(); };
        const std::vector<std::string> toml{"--config", file("s3-credentials.toml")};
        const Environment env{"env-access", "env-secret", "env-token"};
        valid({}, {}, "", "");
        valid(toml, {}, "file-access", "file-secret", "file-token");
        valid({}, env, "env-access", "env-secret", "env-token");
        valid(toml, env, "env-access", "env-secret", "env-token");
        valid({"--config", file("s3-credentials.toml"), "--s3-access-key=cli-access", "--s3-secret-key", "cli-secret", "--s3-session-token=cli-token"},
              env, "cli-access", "cli-secret", "cli-token");
        valid({"--s3-access-key=cli-access"}, {{}, "env-secret", {}}, "cli-access", "env-secret");
        valid({"--config", file("s3-credentials.toml"), "--s3-access-key=cli-access"},
              {{}, "env-secret", {}}, "cli-access", "env-secret", "file-token");
        valid({"--s3-access-key=a", "--s3-secret-key=s", "--s3-session-token=t"}, {"", "", ""}, "a", "s", "t");
        valid({"--config", file("s3-empty-credentials.toml")}, env, "env-access", "env-secret", "env-token");
        invalid({"--config", file("s3-empty-credentials.toml")});
        invalid({"--config", file("s3-unknown.toml")});
        invalid({"--s3-access-key=a"});
        invalid({"--s3-secret-key=do-not-disclose-this-value"});
        invalid({"--s3-session-token=do-not-disclose-this-value"});
        invalid({}, {"", "s", {}});
        invalid({}, {"a", "", {}});
        invalid({}, {"a", "s", ""});
        invalid({"--s3-secret-key=do-not-disclose-this-value", "--s3-secret-key=other"});
        invalid({"--s3-secret-key-typo=do-not-disclose-this-value"});
        invalid({"--s3-access-key"});
        for (const auto* flag : {"--s3-access-key", "--s3-secret-key", "--s3-session-token"}) {
            invalid({flag, ""});
            invalid({"--help", flag, ""});
            invalid({flag, std::string("do-not-disclose-this-value\0", 26)});
            invalid({flag, "do-not-disclose-this-value\n"});
        }
        for (const auto* action : {"--help", "--version"}) {
            valid({action, "--config", "missing.toml"}, {"", "", ""}, "", "");
            const auto result = load({action, "--s3-secret-key=do-not-disclose-this-value"});
            const auto* options = std::get_if<sipi::StartupOptions>(&result);
            check(options && options->help_text.find("do-not-disclose-this-value") == std::string::npos,
                  "Help leaked a credential or rejected a partial pair");
        }
        const std::vector<std::string> repository_file{"--config", file("s3-repository.toml")};
        const auto repository_result = load(repository_file);
        const auto& settings = std::get<sipi::StartupOptions>(repository_result).config;
        check(settings.iiif_storage == sipi::StorageKind::s3 && settings.s3_bucket == "file-bucket" &&
              settings.s3.region == "eu-central-1" && settings.s3.endpoint == "http://localhost:9000" &&
              settings.s3.path_style, "Repository TOML values");
        const sipi::RepositoryEnvironment repository_env{"local", "env-bucket", "eu-west-1", "https://env.test", "false"};
        const auto env_result = load(repository_file, {}, repository_env);
        const auto& es = std::get<sipi::StartupOptions>(env_result).config;
        check(es.iiif_storage == sipi::StorageKind::local && es.s3_bucket == "env-bucket" &&
              es.s3.region == "eu-west-1" && es.s3.endpoint == "https://env.test" && !es.s3.path_style,
              "Repository environment overrides file");
        auto repository_cli = repository_file;
        for (const auto* arg : {"--iiif-storage=s3", "--s3-bucket=cli-bucket", "--s3-region=us-east-1",
                               "--s3-endpoint=http://127.0.0.1:9000", "--s3-path-style=true"})
            repository_cli.emplace_back(arg);
        const auto cli_result = load(repository_cli, {}, repository_env);
        const auto& cs = std::get<sipi::StartupOptions>(cli_result).config;
        check(cs.iiif_storage == sipi::StorageKind::s3 && cs.s3_bucket == "cli-bucket" &&
              cs.s3.region == "us-east-1" && cs.s3.endpoint == "http://127.0.0.1:9000" && cs.s3.path_style,
              "Repository CLI overrides environment and file");
        invalid({"--iiif-storage=s3"});
        for (const auto* arg : {"--iiif-storage=unknown", "--s3-bucket=", "--s3-bucket=ab", "--s3-bucket=a/b",
                               "--s3-bucket=a..b", "--s3-region=", "--s3-region=bad region", "--s3-path-style=yes",
                               "--s3-endpoint=", "--s3-endpoint=http://user:secret@host", "--s3-endpoint=http://host/",
                               "--s3-endpoint=http://host/path", "--s3-endpoint=https://host?q=1", "--s3-endpoint=http://host:0"})
            invalid({arg});
        for (const auto& bad : {sipi::RepositoryEnvironment{"", {}, {}, {}, {}}, {{}, "", {}, {}, {}},
                    {{}, {}, "", {}, {}}, {{}, {}, {}, "", {}}, {{}, {}, {}, {}, ""}}) {
            check(std::holds_alternative<sipi::ParseError>(load({}, {}, bad)), "Empty repository env must fail");
            check(std::holds_alternative<sipi::StartupOptions>(load(repository_cli, {}, bad)), "CLI overrides bad env");
            check(std::holds_alternative<sipi::StartupOptions>(load({"--help"}, {}, bad)), "Help skips bad env");
        }
        check(std::holds_alternative<sipi::StartupOptions>(load({"--help", "--iiif-storage=s3"})), "Help skips bucket requirement");
        std::cout << "S3 configuration precedence, validation and redaction passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
