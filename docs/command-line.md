# Startup configuration contract

The working implementation uses CLI11 in `src/CommandLine.cpp`.
`load_startup_options` returns either owning `StartupOptions` or `ParseError`.
`main` supplies argument views (excluding argv[0]) and an optional snapshot of
`SIPI_LOG_LEVEL`, `SIPI_PORT`, `SIPI_SIZE_UPSCALING`, `SIPI_PREFLIGHT_SCRIPT`,
`SIPI_IIIF_ROOT`, `SIPI_PUBLIC_BASE_URL`, the three `SIPI_CACHE_*` settings, and the three `SIPI_S3_*` credential variables. The loader reads an explicitly selected file, but does not log,
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
- `--cache-dir PATH`: optional persistent cache directory; see the cache section below.
- `--cache-max-bytes BYTES`: encoded file limit, default `1073741824` (1 GiB).
- `--cache-target-percent PERCENT`: cleanup target from 1 to 99, default `90`.
- `--public-base-url URL`: public HTTP(S) origin and optional proxy mount; see below.
- `SIPI_PUBLIC_BASE_URL` and TOML `public-base-url` supply the same setting.
- S3 credential options are listed in the table below.
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

The supported file schema has seventeen root keys: `log-level`, `port`, `size-upscaling`,
`preflight-script`, `iiif-root`, `public-base-url`, `s3-access-key`, `s3-secret-key`,
`s3-session-token`, `iiif-storage`, `s3-bucket`, `s3-region`, `s3-endpoint`,
`s3-path-style`, `cache-dir`, `cache-max-bytes`, and `cache-target-percent`. Unknown keys
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
Diagnostics go to stderr and main returns 2. CLI11 parse errors expose only the
error category, not raw values (which could contain credentials, including in
unknown/duplicate options). File I/O errors still identify the configuration path. The loader retains no input views.
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

Size upscaling is consumed by the geometry API. HTTP JPEG/PNG delivery supports
unchanged dimensions and area-weighted reductions; enlargement rendering remains
unimplemented even when permitted by policy. See
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

## S3 credentials

| Command line | Environment | TOML root key | Default |
| --- | --- | --- | --- |
| `--s3-access-key KEY` | `SIPI_S3_ACCESS_KEY` | `s3-access-key` | Absent |
| `--s3-secret-key SECRET` | `SIPI_S3_SECRET_KEY` | `s3-secret-key` | Absent |
| `--s3-session-token TOKEN` | `SIPI_S3_SESSION_TOKEN` | `s3-session-token` | Absent |

Each parameter follows **CLI > environment > TOML > absent**, exactly like the
other settings. Values are stored as owning strings in `ServerConfig.s3`, the
existing `S3Config` type accepted by `S3Client`. Loading credentials does not create
an SDK client or access the network. The HTTP worker uses this configuration for S3 sources; see repository selection below.

Selected values must be nonempty and contain no ASCII control bytes (including
NUL, CR and LF). Values are not trimmed or normalized. Empty values are errors,
not a way to clear a lower-priority value. Overridden invalid values are ignored.
Duplicate scalar options remain errors. After resolving all sources, access and
secret keys must both be present or both absent; a token requires the pair.
Fields can come from different sources, including the token: when replacing
credentials, also update/remove any stale token in a lower-priority source.

Help/version still validate supplied CLI field syntax, but skip environment,
file loading and pair completeness checks. Thus `--help --s3-secret-key example`
works without an access key, while an explicitly empty CLI value is rejected.
Help and parser diagnostics do not print credential values. Avoid logging the
resolved ServerConfig. CLI values may remain visible in shell history/process
arguments; environment or a protected config file is preferable for real secrets.

If all three SIPI settings are absent, the adapter uses the AWS SDK's normal
credential provider chain, which can include AWS environment variables, profiles
and roles. `AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY` and `AWS_SESSION_TOKEN`
are SDK inputs, not another layer of the SIPI configuration precedence. Explicit
SIPI credentials replace the provider chain as a complete credential set; an
incomplete SIPI pair never borrows a missing field from AWS variables. Explicit
temporary credentials are a startup snapshot and are not automatically refreshed.

Local MinIO example (public test credentials):

```sh
SIPI_S3_ACCESS_KEY=sipi-test SIPI_S3_SECRET_KEY=sipi-local-test-password ./build/Debug/sipi-2
```

Or in an explicitly selected TOML file:

```toml
s3-access-key = "sipi-test"
s3-secret-key = "sipi-local-test-password"
# s3-session-token = "only-for-temporary-credentials"
```

`tests/S3ConfigurationTests.cpp` covers all source priorities, mixed-source pairs,
empty/control values, help/version behavior and diagnostic redaction. The HTTP
process test also checks main's actual SIPI_S3_* environment snapshot. See
[S3 byte sources](s3-byte-source.md) for direct adapter use and MinIO tests.

## Repository selection: local or S3

| Command line | Environment | TOML root key | Default |
| --- | --- | --- | --- |
| `--iiif-storage` | `SIPI_IIIF_STORAGE` | `iiif-storage` | `local` |
| `--s3-bucket` | `SIPI_S3_BUCKET` | `s3-bucket` | Absent |
| `--s3-region` | `SIPI_S3_REGION` | `s3-region` | `us-east-1` |
| `--s3-endpoint` | `SIPI_S3_ENDPOINT` | `s3-endpoint` | AWS regional endpoint |
| `--s3-path-style` | `SIPI_S3_PATH_STYLE` | `s3-path-style` | `false` |

All five fields use **CLI > environment > TOML > default**. `iiif-storage` accepts
exactly `local` or `s3`; `s3-path-style` accepts exactly `true` or `false`.
S3 default storage requires a bucket. Bucket input is 3–63 lowercase ASCII
letters/digits/dots/hyphens, with alphanumeric ends and no empty labels or
hyphens adjacent to dots; provider-specific naming rules/existence are checked by
the service. Region input uses lowercase alphanumeric segments separated by
hyphens. Endpoint is an HTTP(S) origin with a valid host/port, without credentials,
path (including a trailing slash), query or fragment. Omit it for AWS. Selected
empty values are errors, not resets. Help/version skip the cross-field bucket
requirement but still check CLI field syntax.

A configured local root is validated at startup, even with S3 as the default.
A configured bucket enables one shared S3 client, also when the default is local
and only Lua selects S3. Bucket existence, object access and credentials are
checked on source access; startup does not probe objects. No S3 client is created
without a bucket. This increment supports one configured S3 backend/bucket.

**The IIIF URL prefix stays outside the default mapping. Project-specific
translation belongs in the preflight script.** For example:

| IIIF path | Local default source | S3 default source |
| --- | --- | --- |
| `/tiled.tif/info.json` | `<iiif-root>/tiled.tif` | `<s3-bucket>`, key `tiled.tif` |
| `/museum/subpart/tiled.tif/info.json` | `<iiif-root>/tiled.tif` | `<s3-bucket>`, key `tiled.tif` |

Prefixes still appear in public IIIF identities and in `request.prefix` for Lua;
they are not automatically prepended to a filesystem path or object key. Keys
and identifiers are not URL-decoded a second time. Local path containment rules
continue to apply; S3 keys are literal object names, not normalized filesystem paths.
See [preflight](preflight.md) for explicit prefix mapping and mixed sources.

A complete local MinIO example is in `examples/minio.toml`:

```sh
./build/Debug/sipi-2 --config examples/minio.toml
```

With `sipi-test-images/tiled.tif` uploaded, both `/tiled.tif/info.json` and
`/museum/tiled.tif/50,30,100,80/max/0/default.jpg` use that same object.

## Persistent result cache

| CLI / TOML key | Environment | Default |
| --- | --- | --- |
| `cache-dir` | `SIPI_CACHE_DIR` | absent (disabled) |
| `cache-max-bytes` | `SIPI_CACHE_MAX_BYTES` | `1073741824` (1 GiB) |
| `cache-target-percent` | `SIPI_CACHE_TARGET_PERCENT` | `90` |

`cache-dir` must be nonempty and contain no NUL; relative paths resolve against
**the process working directory**, not the TOML file location. The server creates
it if necessary. Use a dedicated private local directory separate from iiif-root.
The database filename is fixed: `<cache-dir>/cache.sqlite3`. The store is retained
across restart; bind a persistent volume when running in a container.

Capacity values use decimal integers (no units/signs/fractions). The byte range is
1..4611686018427387903; percent is 1..99. The byte budget counts encoded bodies and
the single in-progress body, excluding database/journal/filesystem overhead.
Admission additionally leaves at least 64 MiB of available filesystem space at
its check; this is headroom, not an exclusive disk reservation. See
[persistent cache](persistent-cache.md) for exact eviction and failure behavior.

Source precedence is per field, including limits when cache-dir is absent.
Help/version validate CLI syntax but neither initialize the cache nor read its
environment settings. Invalid selected settings fail startup; runtime storage
initialization failure logs a warning and allows uncached operation.

Startup initializes, recovers and locks the store. JPEG/PNG image GET/HEAD requests
now reuse complete encoded results after current preflight, source/version, header,
geometry and capability checks. Hits skip pixel decoding/processing/encoding;
header reads are not cached yet. Storage failures fall back to uncached delivery.
See [HTTP cache integration](persistent-cache.md#http-integration).
