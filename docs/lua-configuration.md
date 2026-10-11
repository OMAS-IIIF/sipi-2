# Script configuration and secrets

Preflight scripts receive two explicitly populated tables:

```lua
local endpoint = sipi.config.api_url
local issuer = sipi.config.jwt_issuer
local key = sipi.secrets.jwt_key
local timeout_ms = tonumber(sipi.config.timeout_ms or "2000")
```

All configured values are strings. Missing entries are `nil`; ordinary settings
may explicitly contain an empty string. Both tables are empty by default. Generic
SIPI configuration does not assign meaning or types to deployment-specific names:
scripts supply domain defaults and validate their own required values.

Only this dedicated namespace is exposed. Environment variables, S3 credentials
and other server settings are not automatically copied into Lua. No `os.getenv`,
filesystem-reading function or unrestricted environment object is added.

## Sources and per-entry priority

Each name resolves independently using **CLI > environment > TOML > empty maps**.
The config and secret namespaces are independent; literal/file entries for the
same secret name share one precedence chain.

| Destination | CLI (repeatable) | Environment variable | TOML root key |
| --- | --- | --- | --- |
| `sipi.config.name` | `--preflight-config name=VALUE` | `SIPI_PREFLIGHT_CONFIG_NAME` | `preflight-config` |
| `sipi.secrets.name`, literal | `--preflight-secret name=VALUE` | `SIPI_PREFLIGHT_SECRETS_NAME` | `preflight-secret` |
| `sipi.secrets.name`, file bytes | `--preflight-secret-file name=PATH` | `SIPI_PREFLIGHT_SECRET_FILES_NAME` | `preflight-secret-file` |

The environment prefixes are intentionally distinct: **CONFIG**, **SECRETS** and
**SECRET_FILES**. Uppercase suffixes normalize to lowercase Lua names. CLI/TOML
names use `[a-z][a-z0-9_]*`, at most 64 characters. Environment suffixes follow the
same rule after ASCII case normalization; case variants colliding on one name
are rejected. Values are never case-normalized or trimmed.

TOML uses arrays of assignments in the existing flat CLI11 configuration style:

```toml
preflight-script = "scripts/access.lua"
preflight-config = [
    "api_url=https://permissions.example.org",
    "jwt_issuer=https://issuer.example.org",
    "jwt_audience=image-service",
    "timeout_ms=2000"
]
preflight-secret-file = ["jwt_key=/run/secrets/image-service-key"]
```

```sh
export SIPI_PREFLIGHT_CONFIG_API_URL='https://staging-permissions.example.org'
./build/Debug/sipi-2 --config sipi.toml --preflight-config timeout_ms=1000
```

In this example only api_url and timeout_ms change; issuer/audience still come
from TOML. A higher-priority literal secret can replace a lower-priority file
reference, and vice versa. Overridden file paths are never opened.

An assignment splits at the **first** `=`. Values may contain further equals
signs, spaces or newlines. Quote the complete assignment in the shell where
necessary. No application-level variable/tilde interpolation is performed.
An empty config value overrides lower-priority values rather than deleting the
entry. Selected empty literal secrets and file paths are invalid.

Duplicate names within one source/namespace are errors, including a literal and
file reference for the same secret at the same priority. Malformed assignments,
duplicate names and structural source limits fail even when another source would
override them. Semantic value/path validation applies to selected winners; as
with existing options, all CLI values are validated even for help/version.

Supplying any script settings without `preflight-script` is a startup error.
Help/version skip that cross-field requirement, environment/TOML loading and all
script/secret-file I/O, while still validating supplied CLI assignments.

## Secret files and lifetime

Prefer mounted secret files or explicitly prefixed environment variables for real
credentials. CLI literals can be visible in process listings/shell history; TOML
literals can accidentally be committed. File references keep the actual bytes
out of command arguments and ordinary configuration files.

Only selected secret files are read, once at startup before script validation or
listening. Paths are relative to the process working directory, **not the TOML
file's directory**; absolute deployment paths are preferable. Symlinks to regular
files are supported, including mounted-secret layouts. FIFOs, directories, empty,
unreadable and oversized files fail; there is no fallback to an older/lower source.
The regular-file check and bounded read share one descriptor to avoid a stat/open
race. File permissions and trusted paths remain deployment responsibilities.

Contents are passed **byte for byte**, including NUL and a final newline. This
supports raw HMAC keys and multiline PEM. For textual shared secrets, a newline
changes the key: use an appropriate secret writer (e.g. `printf` rather than an
unintended `echo` newline). No trimming, PEM interpretation or decoding occurs in
the configuration loader; JWT validates its own key requirements.

`LuaPreflight` owns a snapshot of script text and resolved values. Changing the
file, environment or original C++ input after startup does not change running
requests. Restart/reconstruct the evaluator to adopt changes. Publish secret
updates atomically; concurrent in-place writes during startup are not a versioned
secret-storage protocol.

Each Lua state receives fresh ordinary tables before top-level initialization,
so startup assertions and helper setup can use them. Scripts can mutate these
local tables, but that affects only that evaluation. It cannot modify the native
snapshot, another request or another worker. These tables are not a shared token
cache. Native and Lua copies of secrets are not guaranteed to be securely zeroized.

## Limits and diagnostics

The combined config/secret snapshot allows 64 entries, at most 16 KiB per value
and 256 KiB total name/value bytes. Secret paths are additionally limited to 4096
bytes. Each source has its own 64-assignment/256-KiB structural input ceiling.
After file loading, limits are checked again against the actual bytes. Text
assignments must not contain NUL; binary secret-file contents may contain it.
Lua's existing allocator and instruction budgets still apply to each state.

Configuration/secret-file errors use fixed messages, without names, values, paths
or environment contents. CLI11 diagnostics remain sanitized, and help does not
print supplied values. Server startup and request logs include the category name,
number and Lua diagnostic (normally script path and line). Startup exits 2;
HTTP clients still receive only the generic `preflight_failed` response.
Current categories are script_file (0), script_load (1), script_execution (2),
contract (3), memory (4), instruction_limit (5).

Before constructing the Lua exception, SIPI masks exact occurrences of configured
secret values, incoming Authorization (including its credential part), raw and
parsed cookies, and decoded query values. Overlapping matches are masked together.
Control bytes are escaped (e.g. `\x0a`) to prevent multiline log injection; the
message is limited to 2048 bytes plus a truncation marker. Non-string Lua errors
receive a fixed diagnostic. This applies to startup and request evaluation.

This is defense in depth, not a guarantee for arbitrary script output: transformed
or partially printed credentials, secrets hardcoded in scripts/config, and tokens
or personal data returned by external APIs cannot reliably be recognized. Never
put these, complete HTTP bodies or headers into `error()`/`assert()` messages.
Use contextual messages such as `error("permission lookup: unexpected HTTP 503")`.
Keep operator logs access-controlled; diagnostics are never suitable client bodies.
Scripts remain trusted operator code, not a sandbox.

## Using configured JWT settings

```lua
local issuer = assert(sipi.config.jwt_issuer, "jwt_issuer is required")
local audience = assert(sipi.config.jwt_audience, "jwt_audience is required")
local key = assert(sipi.secrets.jwt_key, "jwt_key is required")

local function verify_token(token)
    return sipi.jwt.verify(token, {
        algorithm = "HS256",
        key = key,
        issuer = issuer,
        audience = audience,
        type = "JWT"
    })
end
```

The script still decides how to extract a token and bind its verified claims to
the requested resource. There is no OLDAP-specific setting, claim or endpoint.
Use RS256 with appropriate public/private PEM settings where needed; see
[the JWT contract](lua-jwt.md).

## Implementation and tests

`ScriptConfig.*` separates pure source parsing/merging (`ScriptSettings`) from
startup secret-file resolution (`ScriptConfig`). Its `scriptconfig` CMake target
has no Lua, HTTP, JSON or codec dependency. `CommandLine` merges collections per
name while retaining CLI11's existing scalar precedence and config-file validation.
`main` snapshots only the selected environment prefixes, resolves files, and
passes the owning values into `LuaPreflight`.

`ScriptConfigTests.cpp` covers precedence, duplicates, limits, help behavior,
regular-file boundaries, exact bytes, snapshots and concurrent isolation.
`script_config_http.py` checks the actual process from CLI/environment/TOML through
Lua/JWT execution, file overrides, restart semantics and startup/runtime redaction.

```sh
ctest --preset conan-debug -R '^(script-configuration|script-config-http)$' --output-on-failure
```

Next: extend the request context with query parameters and an explicit IIIF
request kind, so generic policies can distinguish information/base/image requests
and consume query tokens without reparsing URLs. Then compose a separate deployment
example using the generic JSON, HTTP, JWT and configuration capabilities.
