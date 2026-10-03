# Startup configuration contract

The working implementation uses CLI11 in `src/CommandLine.cpp`.
`load_startup_options` returns either owning `StartupOptions` or `ParseError`.
`main` supplies argument views (excluding argv[0]) and an optional snapshot of
`SIPI_LOG_LEVEL`, `SIPI_PORT`, `SIPI_SIZE_UPSCALING`, `SIPI_PREFLIGHT_SCRIPT`,
`SIPI_IIIF_ROOT`, and `SIPI_PUBLIC_BASE_URL`. The loader reads an explicitly selected file, but does not log,
print, exit, or change logging/environment state.

## Sources and precedence

For each setting: **command line > environment > file > default**.
The resolved `ServerConfig` contains concrete values and optional preflight and repository paths. CLI11 tracks whether
an option was explicitly supplied, so `--log-level info` overrides a file value
just as any other explicit level does. An absent preflight path disables script loading.

The loader first parses CLI arguments. For ordinary startup, it supplies the
injected environment value to the same option/validator only if CLI omitted it.
It then uses `parse_from_stream` to fill still-missing options from the file.
This order is deliberate: CLI11's automatic `set_config`/`envname` processing
would put the file ahead of the environment in the version we use.

## Supported input

- `--help`: print CLI11-generated help; exit 0 without startup logging.
- `--version`: print the CMake-defined application version; exit 0 without startup logging.
- `--log-level LEVEL`: select `trace`, `debug`, `info`, `warn`, `error`, `critical`, or `off`.
- `--port PORT`: local HTTP port, an integer from 1 to 65535.
- `--size-upscaling BOOL`: permit IIIF enlargement; exactly `true` or `false`.
- `--preflight-script PATH`: optional Lua script, nonempty and without NUL bytes.
- `--iiif-root PATH`: optional repository directory, nonempty and without NUL bytes.
- `--public-base-url URL`: public HTTP(S) origin and optional proxy mount; see below.
- `SIPI_PUBLIC_BASE_URL` and TOML `public-base-url` supply the same setting.
- `--config PATH`: load one explicitly selected configuration file.
- `SIPI_LOG_LEVEL`: the same case-sensitive level names; empty is invalid if selected.
- `SIPI_PORT`: the same port range; empty is invalid if selected.
- `SIPI_SIZE_UPSCALING`: exactly `true` or `false`; empty is invalid if selected.
- File: `log-level = "warn"` and `port = 9090`, for example. No file is loaded implicitly.
- File also supports `size-upscaling = true` or `false`.
- `SIPI_PREFLIGHT_SCRIPT`: optional Lua path with the same validation.
- File also supports `preflight-script = "scripts/preflight.lua"`.
- `SIPI_IIIF_ROOT`: repository path, with the same validation.
- File also supports `iiif-root = "data/images"`.
- Defaults: log level `info`, port `8080`, size upscaling `false`, no preflight script, no repository root.

CLI11 also accepts equals syntax (`--log-level=debug`, `--config=path`).
There are no short option aliases, positional arguments, or `--run` option.
Duplicate scalar options/flags and conflicting help/version actions are errors.
CLI11's standard `--` end-of-options marker is accepted; positional tokens after
it remain errors. Flag value overrides such as `--help=false` are rejected.

The supported file schema has six root keys: `log-level`, `port`, `size-upscaling`,
`preflight-script`, `iiif-root`, and `public-base-url`. Unknown keys
and startup actions in the file are rejected. CLI11's built-in TOML-style reader
is used directly; this is not a promise of full TOML grammar validation (including
all malformed syntax or duplicate-key checks). Revisit the parser if the schema
needs full TOML semantics rather than these simple settings.

## Errors and overrides

All CLI input is validated, even for help/version. Once CLI parsing succeeds,
help/version bypass environment and file loading. Thus a missing file does not
prevent help, but `--help --unknown` is still an error.

For normal startup, an explicitly requested file must be readable even if CLI
supplies every setting. Unknown file keys remain errors. A lower-priority setting
value that is overridden is not semantically validated; the selected value is.
An invalid selected value fails startup instead of silently reverting to defaults.
Diagnostics go to stderr and main returns 2. The loader retains no input views.
Allocation failures may propagate as exceptions.

## Representative outcomes

| Input | Result |
| --- | --- |
| No sources | run, info |
| File warn | run, warn |
| File warn + environment debug | run, debug |
| File warn + environment debug + CLI error | run, error |
| File warn + CLI info | run, info |
| `--log-level debug --version` | version output, exit 0 |
| `--help --config missing.toml` | help output, exit 0 |
| `--log-level` | error, exit 2 |
| `--help --version` | error, exit 2 |
| `--log-level DEBUG` | error, exit 2 |
| `--log-level 2` | error, exit 2 |
| `--log-level info --log-level debug` | error, exit 2 |
| Invalid selected environment level | error, exit 2 |
| `--config missing.toml` | error, exit 2 |

## Tests and evolution

`tests/CommandLineTests.cpp` retains the manual parser's acceptance cases, updates
those intentionally changed by CLI11, and adds source-priority/file cases.
Run via `ctest --preset conan-debug --output-on-failure` after building.
The previous hand-written parser was reviewed successfully before replacement.
Future configuration fields belong in `ServerConfig` and must have explicit
validation and precedence tests. No generic configuration registry is needed.

Size upscaling is consumed by the geometry API; current HTTP JPEG delivery still
requires unchanged crop dimensions because resizing is not implemented. See
[TIFF metadata and geometry](tiff-metadata-and-geometry.md).

## Preflight startup initialization

The loader validates only path syntax and precedence, without opening the script.
Relative paths use the process working directory regardless of configuration source;
paths from TOML are not relative to that TOML file. Use absolute paths in deployment.
No tilde or environment-variable expansion is performed by the application.

After configuration/help/version handling, main loads a configured script once and
calls `LuaPreflight::validate()` before opening the listener. Validation compiles
and executes top-level initialization, then checks that `preflight` is a function;
it does not call the request function. Unreadable files, syntax errors, initialization
errors, or a missing function print a diagnostic to stderr and exit 2, including
when logging is off. There is no fallback to running without the configured script.
Help/version skip script I/O but still reject invalid CLI path syntax.

The script snapshot lives for the server lifetime; changes require a restart.
Request-dependent failures remain detectable only during evaluation. Trusted script
initialization has the same Lua memory/instruction budgets as request evaluation.
Native-call wall-clock limits remain separate. A configured script authorizes IIIF
image/info GET and HEAD requests on the worker before source lookup. Health and
hello are outside this policy. Without a script, delivery uses the repository's default mapping.
See [the preflight contract](preflight.md).

## Repository root

The optional iiif_root field is a filesystem path. Server initialization validates
and canonicalizes an explicitly configured existing directory before listening;
missing/non-directory roots exit 2. Relative root paths use cwd. Request resolution
uses root/identifier by default (without prefix); relative Lua source overrides
also use root. Only absolute Lua source overrides work without a root. This changes
the previous cwd-based relative-source rule. See [source resolution](source-resolution.md).

## Public service address

`--public-base-url`, `SIPI_PUBLIC_BASE_URL`, and TOML `public-base-url` follow the
same precedence. When absent, HTTP uses `http://127.0.0.1:<configured-port>`.
Supply the public HTTPS address behind Caddy. Host, Forwarded and X-Forwarded-*
headers cannot change the service identity. The value is independent of iiif-root.

The loader accepts an absolute lowercase `http://` or `https://` URL with an ASCII
hostname (use punycode for international names), IPv4 or bracketed IPv6, an optional
port in 1..65535, and an already percent-encoded path. It rejects credentials,
query/fragment, spaces, malformed escapes, controls, and empty/dot path segments.
It removes one trailing slash. IPv6 syntax uses the platform's inet_pton (macOS/Linux).
No DNS lookup or network request occurs. An empty selected value is an error.

An optional mount path represents only a prefix **removed by the reverse proxy**.
If Caddy forwards `/museum/id/info.json` unchanged, configure only the origin;
the parsed `museum` prefix is appended automatically. If it strips `/iiif` from
`/iiif/museum/id/info.json`, configure `https://images.example.org/iiif`.
See [information responses](iiif-info.md) for examples and development boundaries.
