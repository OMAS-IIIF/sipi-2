#include "preflight/ScriptConfig.h"

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace sipi::preflight {
namespace {
constexpr std::size_t entry_limit = 64;
constexpr std::size_t value_limit = 16 * 1024;
constexpr std::size_t total_limit = 256 * 1024;
void require(bool ok, const char* message) {
    if (!ok) throw ScriptConfigError(message);
}
bool name_valid(std::string_view name) {
    if (name.empty() || name.size() > 64 || name.front() < 'a' || name.front() > 'z') return false;
    for (char c : name) if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}
struct Budget {
    std::size_t entries = 0, bytes = 0;
    void add(std::string_view name, std::string_view value) {
        require(name_valid(name), "Invalid preflight setting name; use lower-case identifiers");
        require(++entries <= entry_limit, "Too many preflight settings (maximum 64)");
        require(value.size() <= value_limit, "Preflight setting exceeds 16 KiB");
        require(name.size() + value.size() <= total_limit - bytes, "Preflight settings exceed 256 KiB");
        bytes += name.size() + value.size();
    }
};

/** Regular-file check and read share a descriptor, avoiding a stat/open race and
 * blocking FIFOs. Symlinks to regular files (e.g. mounted secrets) are allowed. */
std::string read_secret(const std::string& path) {
    struct File {
        int fd;
        ~File() { if (fd >= 0) ::close(fd); }
    } file{::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK)};
    require(file.fd >= 0, "Cannot open preflight secret file");
    struct stat info{};
    require(::fstat(file.fd, &info) == 0 && S_ISREG(info.st_mode), "Preflight secret file must be a readable regular file");
    require(info.st_size > 0 && info.st_size <= static_cast<off_t>(value_limit), "Preflight secret file must contain 1..16384 bytes");
    std::array<char, value_limit + 1> bytes{};
    std::size_t used = 0;
    while (used < bytes.size()) {
        const auto count = ::read(file.fd, bytes.data() + used, bytes.size() - used);
        if (count < 0 && errno == EINTR) continue;
        require(count >= 0, "Cannot read preflight secret file");
        if (count == 0) break;
        used += static_cast<std::size_t>(count);
    }
    require(used > 0 && used <= value_limit, "Preflight secret file must contain 1..16384 bytes");
    return {bytes.data(), used};
}
}

ScriptSettings parse_script_settings(const std::vector<std::string>& config,
                                     const std::vector<std::string>& secrets,
                                     const std::vector<std::string>& secret_files) {
    ScriptSettings result;
    std::size_t source_bytes = 0, entries = 0;
    const auto add = [&](const std::vector<std::string>& list, int kind) {
        for (const auto& entry : list) {
            require(++entries <= entry_limit && entry.size() <= total_limit - source_bytes,
                    "Preflight setting source exceeds entry/byte limits");
            source_bytes += entry.size();
            const auto separator = entry.find('=');
            require(separator != std::string::npos && name_valid(std::string_view(entry).substr(0, separator)),
                    "Preflight setting must use name=value with a lower-case identifier");
            const auto name = entry.substr(0, separator);
            const auto value = entry.substr(separator + 1); // Keep all further '=' and whitespace.
            if (kind == 0) require(result.config.emplace(name, value).second, "Duplicate preflight configuration name");
            else require(result.secrets.emplace(name, SecretSetting{value, kind == 1 ? SecretSetting::Source::literal : SecretSetting::Source::file}).second,
                         "Duplicate preflight secret name or conflicting literal/file sources");
        }
    };
    add(config, 0); add(secrets, 1); add(secret_files, 2);
    return result;
}

ScriptSettings parse_script_environment(std::span<const std::string_view> entries) {
    std::vector<std::string> config, secrets, files;
    std::size_t source_bytes = 0, count = 0;
    for (auto entry : entries) {
        std::vector<std::string>* target = nullptr;
        // Distinct prefixes avoid collisions with secret names such as file_key.
        for (const auto& [prefix, list] : {
                std::pair{"SIPI_PREFLIGHT_SECRET_FILES_", &files},
                std::pair{"SIPI_PREFLIGHT_SECRETS_", &secrets},
                std::pair{"SIPI_PREFLIGHT_CONFIG_", &config}}) {
            if (entry.starts_with(prefix)) { entry.remove_prefix(std::string_view(prefix).size()); target = list; break; }
        }
        if (!target) continue;
        require(++count <= entry_limit && entry.size() <= total_limit - source_bytes,
                "Preflight environment exceeds entry/byte limits");
        source_bytes += entry.size();
        std::string normalized(entry);
        const auto separator = normalized.find('=');
        for (std::size_t i = 0; i < normalized.size() && i != separator; ++i)
            if (normalized[i] >= 'A' && normalized[i] <= 'Z') normalized[i] += 'a' - 'A';
        target->push_back(std::move(normalized));
    }
    return parse_script_settings(config, secrets, files);
}

void validate_script_settings(const ScriptSettings& settings) {
    Budget budget;
    for (const auto& [name, value] : settings.config) {
        budget.add(name, value);
        require(value.find('\0') == std::string::npos, "Preflight text settings must not contain NUL");
    }
    for (const auto& [name, secret] : settings.secrets) {
        budget.add(name, secret.value);
        require(!secret.value.empty() && secret.value.find('\0') == std::string::npos,
                "Preflight secret value/path must be nonempty without NUL");
        if (secret.source == SecretSetting::Source::file)
            require(secret.value.size() <= 4096, "Preflight secret path exceeds 4096 bytes");
    }
}
ScriptSettings merge_script_settings(ScriptSettings cli, const ScriptSettings& environment,
                                     const ScriptSettings& file) {
    for (const auto* source : {&environment, &file}) {
        cli.config.insert(source->config.begin(), source->config.end());
        cli.secrets.insert(source->secrets.begin(), source->secrets.end());
    }
    validate_script_settings(cli);
    return cli;
}
void validate_script_config(const ScriptConfig& config) {
    Budget budget;
    for (const auto& [name, value] : config.config) {
        budget.add(name, value);
        require(value.find('\0') == std::string::npos, "Preflight text settings must not contain NUL");
    }
    for (const auto& [name, value] : config.secrets) {
        budget.add(name, value);
        require(!value.empty(), "Preflight secret must not be empty");
    }
}
ScriptConfig resolve_script_config(const ScriptSettings& settings) {
    validate_script_settings(settings);
    ScriptConfig result{settings.config, {}};
    for (const auto& [name, secret] : settings.secrets)
        result.secrets.emplace(name, secret.source == SecretSetting::Source::file ? read_secret(secret.value) : secret.value);
    validate_script_config(result);
    return result;
}
}
