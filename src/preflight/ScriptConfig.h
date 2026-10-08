#pragma once

#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace sipi::preflight {
using ScriptMap = std::map<std::string, std::string>;

/** A selected secret is either literal bytes or a path resolved at startup.
 * No source is read during option parsing; overridden paths are never opened. */
struct SecretSetting {
    enum class Source { literal, file };
    std::string value;
    Source source = Source::literal;
};
struct ScriptSettings {
    ScriptMap config;
    std::map<std::string, SecretSetting> secrets;
    [[nodiscard]] bool empty() const noexcept { return config.empty() && secrets.empty(); }
};

/** Owning startup snapshot. Never log either map. Each Lua state receives its
 * own copy; script mutations do not change this snapshot or other evaluations. */
struct ScriptConfig {
    ScriptMap config;
    ScriptMap secrets;
};

/** Fixed diagnostics never contain names, paths, values or environment entries. */
class ScriptConfigError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/** Parse a single source's NAME=VALUE lists. Names are lower-case identifiers;
 * duplicate names within each namespace (including literal/file collisions) fail.
 * Structural source limits always apply; value validation follows precedence. */
ScriptSettings parse_script_settings(const std::vector<std::string>& config,
                                     const std::vector<std::string>& secrets,
                                     const std::vector<std::string>& secret_files);
/** Read only explicitly prefixed NAME=VALUE entries from a caller-owned snapshot.
 * Unrelated variables are ignored. Suffixes normalize ASCII case to lower case. */
ScriptSettings parse_script_environment(std::span<const std::string_view> entries);
/** Merge per entry: CLI > environment > file > empty maps, then validate winners.
 * No file I/O; help/version can validate only CLI via validate_script_settings. */
ScriptSettings merge_script_settings(ScriptSettings cli, const ScriptSettings& environment,
                                     const ScriptSettings& file);
void validate_script_settings(const ScriptSettings& settings);
/** Read only selected regular secret files, preserving exact bytes (no trimming).
 * Limits: 64 combined entries, 16 KiB/value, 256 KiB total including names.
 * Relative paths use process cwd; reconstruct the snapshot to adopt file changes. */
ScriptConfig resolve_script_config(const ScriptSettings& settings);
void validate_script_config(const ScriptConfig& config);
}
