# Native configuration and option reference

SIPI-2 can run directly as a native executable, without Docker. Configure the
listener, repository and optional Lua policy at startup using command-line
arguments, environment variables or one explicitly selected TOML file. This page
is the shared option reference for native and container operation; see the
[Docker guide](docker.md) for mounts and container-specific defaults.

## Quickstart without Docker

After following the [build guide](building.md), run from the repository root:

```sh
./build/Debug/sipi-2 --config examples/native/sipi.toml
```

The [native example](../examples/native/sipi.toml) binds to `127.0.0.1:18080`, serves
[the synthetic image](../examples/images/README.md), and selects two workers.
No Lua policy or persistent cache is enabled. In another terminal:

```sh
curl --fail http://127.0.0.1:18080/health
curl --fail http://127.0.0.1:18080/sample.png/info.json
curl --fail http://127.0.0.1:18080/sample.png/full/160,/0/default.jpg -o preview.jpg
```

The source is 320 × 200 and the preview 160 × 100. Stop with Ctrl-C. Use the Release
binary at `build/Release/sipi-2` after a Release build. Stop other demos using port
18080 first. For your own collection, copy the configuration and set `iiif-root`
to its existing directory. Without a policy, supported resolvable images in that
repository are accessible.

To enable the optional persistent result cache, restart with:

```sh
./build/Debug/sipi-2 --config examples/native/sipi.toml \
  --cache-dir build/native-demo-cache
```

The server creates this dedicated cache directory if possible. Original images
remain in `examples/images`; the cache is separate and regenerable.

## Sources and precedence

Each setting resolves independently in this order:

**CLI > environment > explicitly selected TOML file > built-in default.**

```sh
SIPI_PORT=18081 ./build/Debug/sipi-2 --config examples/native/sipi.toml \
  --port 18082 --public-base-url http://127.0.0.1:18082
```

This starts on 18082: CLI beats environment 18081 and file 18080. The public URL
is overridden separately because it is an independent setting. Remove `--port`
to use the environment value; remove both overrides to use the file value. Adjust
the public URL accordingly. An explicit value equal to a default still overrides
lower-priority values.

No configuration file is searched for implicitly and there is no `SIPI_CONFIG`
environment setting. Select a file with `--config PATH`. Environment changes in
another shell do not affect a running server. Configuration, Lua scripts and
secret-file contents are startup snapshots: restart to adopt changes. There is no
hot reload, `--check-config` mode or effective-configuration dump.

Empty selected scalar values normally fail; they do not clear lower-priority
settings. To disable optional file-cache or policy configuration, remove its value
from all applicable sources rather than assigning an empty path. Booleans use
exactly `true` or `false`, not `1`, `yes` or a valueless switch. Lua maps have their
own per-name merge rules described below.

## Paths, permissions and service startup

All relative filesystem paths are based on the **process working directory**, not
the TOML file's directory. This includes `--config` itself, `iiif-root`, Lua script
paths, secret files and cache paths. Relative *source paths returned by Lua* are a
separate case: they resolve within `iiif-root`. There is no application-level
expansion of `~`, `$VARIABLE` or `${VARIABLE}` in configuration values; your shell
may expand unquoted CLI input before SIPI receives it.

For a system service, use absolute paths or explicitly set its working directory.
Run it as an account that can read image/configuration/policy files and traverse
their parent directories. Only its cache and working/runtime directories need
writes for ordinary delivery. Drogon may create an `uploads/` directory in the
working directory; this does not enable an upload API. A native user does not need
the container's UID 10001.

Repository roots must already exist. Use a dedicated writable cache directory,
separate from the image repository and private to one running SIPI instance. Do
not share it between processes or over network storage. Keep port/bind settings
separate from public URL configuration; HTTPS normally belongs to a reverse proxy.

## Complete option table

For each row below, the first column is the **TOML root key**; prefix it with `--`
for the corresponding CLI option. For example, `jpeg-quality = 85`,
`SIPI_JPEG_QUALITY=85` and `--jpeg-quality 85` address the same setting.
Quoted strings, booleans and decimal integers are used in the flat TOML schema.
Three command-only options are separate:

| CLI only | Meaning |
| --- | --- |
| `--help` | Print supported options and exit 0; not a configuration validation mode |
| `--version` | Print the CMake-defined version and exit 0 |
| `--config PATH` | Load one explicit file; no environment or TOML equivalent |

### Listener and public HTTP behavior

| TOML key / CLI name | Environment | Default | Type, values and purpose |
| --- | --- | --- | --- |
| `log-level` | `SIPI_LOG_LEVEL` | `info` | String: trace, debug, info, warn, error, critical, off; application log severity |
| `listen-address` | `SIPI_LISTEN_ADDRESS` | `127.0.0.1` | Numeric IPv4/IPv6 address, no hostname/port/brackets/zone suffix |
| `port` | `SIPI_PORT` | `8080` | Integer 1–65535; listener port |
| `public-base-url` | `SIPI_PUBLIC_BASE_URL` | `http://127.0.0.1:<port>` | Absolute HTTP(S) public origin and optional encoded proxy mount |
| `cors-origins` | `SIPI_CORS_ORIGINS` | `*` | One string: wildcard or comma-separated exact HTTP(S) origins, without spaces/paths |
| `cors-credentials` | `SIPI_CORS_CREDENTIALS` | `false` | Boolean; true requires explicit origins, not wildcard |

The shipped Docker image sets `SIPI_LISTEN_ADDRESS=0.0.0.0`; that environment value
also overrides a TOML bind setting. Native SIPI has no such injected default.

### Repository and S3

| TOML key / CLI name | Environment | Default | Type, values and purpose |
| --- | --- | --- | --- |
| `iiif-root` | `SIPI_IIIF_ROOT` | Absent | Nonempty path to an existing local repository; also resolves relative Lua sources |
| `iiif-storage` | `SIPI_IIIF_STORAGE` | `local` | String: local or s3; default source mapping |
| `s3-bucket` | `SIPI_S3_BUCKET` | Absent | Bucket name, 3–63 characters under the validation rules below; required for S3 sources |
| `s3-region` | `SIPI_S3_REGION` | `us-east-1` | Lowercase alphanumeric segments separated by hyphens |
| `s3-endpoint` | `SIPI_S3_ENDPOINT` | AWS regional endpoint | HTTP(S) origin only; no path, trailing slash, credentials, query or fragment |
| `s3-path-style` | `SIPI_S3_PATH_STYLE` | `false` | Boolean; typically true for local MinIO |
| `s3-access-key` | `SIPI_S3_ACCESS_KEY` | Absent | Nonempty credential string; must be paired with secret key |
| `s3-secret-key` | `SIPI_S3_SECRET_KEY` | Absent | Nonempty credential string; must be paired with access key |
| `s3-session-token` | `SIPI_S3_SESSION_TOKEN` | Absent | Nonempty temporary token; requires the key pair |

### Image output and concurrency

| TOML key / CLI name | Environment | Default | Type, values and purpose |
| --- | --- | --- | --- |
| `size-upscaling` | `SIPI_SIZE_UPSCALING` | `false` | Boolean; actual enlargement also requires an IIIF `^` size |
| `max-width` | `SIPI_MAX_WIDTH` | `8192` | Decimal integer 1–32768; output width before rotation |
| `max-height` | `SIPI_MAX_HEIGHT` | Resolved max-width | Decimal integer 1–32768; inherits width only when absent from every source |
| `max-area` | `SIPI_MAX_AREA` | `8388608` | Decimal integer 1–8388608; output pixel-area ceiling |
| `jpeg-quality` | `SIPI_JPEG_QUALITY` | `90` | Decimal integer 1–100; JPEG quantization, not a lossless mode |
| `jpeg-sampling` | `SIPI_JPEG_SAMPLING` | `4:4:4` | String: 4:4:4 or 4:2:0; ignored for one-channel JPEG |
| `bitonal-method` | `SIPI_BITONAL_METHOD` | `threshold` | String: threshold or floyd-steinberg |
| `iiif-workers` | `SIPI_IIIF_WORKERS` | Logical CPUs clamped to 1–4 | Decimal integer 1–32; image workers, not an automatic memory quota |

### Cache capacity

| TOML key / CLI name | Environment | Default | Type, values and purpose |
| --- | --- | --- | --- |
| `cache-dir` | `SIPI_CACHE_DIR` | Absent; disabled | Nonempty dedicated local path; created if needed |
| `cache-max-bytes` | `SIPI_CACHE_MAX_BYTES` | `1073741824` | Decimal integer 1–4611686018427387903; encoded-file budget, excluding DB/overhead |
| `cache-target-percent` | `SIPI_CACHE_TARGET_PERCENT` | `90` | Decimal integer 1–99; target occupancy after eviction |
| `metadata-cache-max-entries` | `SIPI_METADATA_CACHE_MAX_ENTRIES` | `256` | Unsigned decimal size_t; source metadata entry capacity |
| `metadata-cache-max-bytes` | `SIPI_METADATA_CACHE_MAX_BYTES` | `67108864` | Unsigned decimal size_t; accounted metadata bytes |
| `mask-cache-max-entries` | `SIPI_MASK_CACHE_MAX_ENTRIES` | `64` | Unsigned decimal size_t; decoded watermark entry capacity |
| `mask-cache-max-bytes` | `SIPI_MASK_CACHE_MAX_BYTES` | `33554432` | Unsigned decimal size_t; accounted mask bytes |
| `variant-cache-max-entries` | `SIPI_VARIANT_CACHE_MAX_ENTRIES` | `16` | Unsigned decimal size_t; reduced-public-raster entry capacity |
| `variant-cache-max-bytes` | `SIPI_VARIANT_CACHE_MAX_BYTES` | `67108864` | Unsigned decimal size_t; accounted public-raster bytes |

For the six `size_t` fields, allowed values are 0–18446744073709551615 on the
supported 64-bit builds. Either zero limit disables that memory cache's retention.
Zero is **not** valid for `cache-max-bytes`; omit `cache-dir` to disable persistence.
These limits do not reserve memory or bound total process RSS. Byte settings take
plain decimal numbers, not units such as `64M` or `1GiB`.

### Lua policy and settings

| TOML key / CLI name | Environment | Default | Type, values and purpose |
| --- | --- | --- | --- |
| `preflight-script` | `SIPI_PREFLIGHT_SCRIPT` | Absent; disabled | Nonempty path to a trusted Lua script |
| `preflight-config` | `SIPI_PREFLIGHT_CONFIG_<NAME>` | Empty map | TOML array / repeatable CLI `name=value`; populates sipi.config |
| `preflight-secret` | `SIPI_PREFLIGHT_SECRETS_<NAME>` | Empty map | TOML array / repeatable CLI `name=value`; literal secret bytes |
| `preflight-secret-file` | `SIPI_PREFLIGHT_SECRET_FILES_<NAME>` | Empty map | TOML array / repeatable CLI `name=path`; secret-file startup snapshot |

Script names use `[a-z][a-z0-9_]*`, up to 64 characters; environment suffixes
normalize to lowercase names. Config and secret maps resolve per name, not by
replacing the whole map. Literal and file secrets share one namespace/priority
chain. Same-source duplicates/collisions fail. Script settings require a script.
An empty ordinary Lua config string is allowed; empty secrets/file paths are not.
See [explicit Lua configuration and secrets](#explicit-lua-configuration-and-secrets).

## Supported input

`--name=value` and `--name value` work. There are no short aliases, positional
arguments or `--run` option. Boolean settings require an explicit value. Scalar
option duplicates and conflicting help/version actions fail; the three Lua
assignment options are repeatable with distinct names. `--` ends option parsing,
but positional tokens after it remain errors. `--help=false` is not supported.

The supported file schema consists of the root keys in the complete table.
Use flat assignments; unknown keys and command-only actions inside a file are
rejected. CLI11's TOML-style reader handles this schema; it is not a claim of full
TOML-spec validation for arbitrary malformed syntax, duplicate keys or structures.
The annotated [root template](../sipi.example.toml) lists settings without enabling
an arbitrary repository or application policy. The [native demo](../examples/native/sipi.toml)
is directly runnable from the repository root.

## Errors and overrides

All explicit CLI input is validated, including with help/version. Once parsing
succeeds, help/version skip environment/file loading, script I/O and server startup.
Consequently `--help --config missing.toml` succeeds, but `--help --unknown` fails.
Help is **not** a way to validate a TOML file. Normal startup checks the selected
configuration and resources before serving; exercise a representative image too,
because `/health` does not establish that every source or policy backend works.

An explicitly selected file must be readable even when CLI overrides all of its
values. Unknown keys still fail. For ordinary scalar settings an overridden value
is not semantically validated; an invalid selected value fails rather than reverting
to defaults. Lua maps additionally reject malformed assignments/duplicates at their
source boundary. Cross-field rules still apply after merging (S3 credentials,
S3 default bucket, CORS credential mode and settings requiring a policy).

Configuration, script initialization and repository startup failures report to
stderr and exit 2. Invalid CLI values are not echoed in parser diagnostics;
configuration-file I/O errors can identify its path. Logs may include bounded,
escaped Lua diagnostics with known-secret masking; do not log transformed secrets
or complete resolved configuration. Cache-storage initialization failure can instead
warn and continue uncached; an invalid cache *setting* still fails parsing.
`log-level=off` does not suppress required startup stderr diagnostics.

## Representative outcomes

| Input | Result |
| --- | --- |
| No repository or policy | Listener can start; default image requests lack a source repository |
| File port + environment port + CLI port | CLI port wins; other settings resolve independently |
| File max-width=1000; no max-height anywhere | Both effective axes are 1000 |
| File max-height=900 + CLI max-width=1200 | Height stays 900 |
| `--help --config missing.toml` | Help, exit 0; file was not validated |
| `--help --unknown` or `--help --version` | Error, exit 2 |
| Selected `SIPI_PORT=invalid` | Error, exit 2 |
| Invalid environment port overridden by valid CLI port | CLI value used |
| Missing configured image root or invalid Lua script | Startup error, exit 2 |
| Script settings without preflight-script | Startup error, exit 2 |

## Preflight startup initialization

The loader validates only path syntax and precedence, without opening the script.
Relative paths use the process working directory regardless of configuration source;
paths from TOML are not relative to that TOML file. Use absolute paths in deployment.
No tilde or environment-variable expansion is performed by the application.

After configuration/help/version handling, main loads a configured script once and
calls `LuaPreflight::validate()` before opening the listener. Validation compiles
and executes top-level initialization, then checks that `preflight` is a function;
it does not call the request function. Unreadable files, syntax errors, initialization
errors, or a missing function print an error category and bounded, escaped,
known-secret-masked diagnostic to stderr and exit 2, including when logging is off. There is no fallback to running without the configured script.
Help/version skip script I/O but still reject invalid CLI path syntax.

The script snapshot lives for the server lifetime; changes require a restart.
Request-dependent failures remain detectable only during evaluation. Trusted script
initialization has the same Lua memory/instruction budgets as request evaluation.
Native-call wall-clock limits remain separate. A configured script authorizes IIIF
image/info/base GET and HEAD requests on the worker before source lookup. Health and
hello are outside this policy. Without a script, delivery uses the repository's default mapping.
See [the preflight contract](preflight.md).

## Repository root

The optional iiif_root field is a filesystem path. Server initialization validates
and canonicalizes an explicitly configured existing directory before listening;
missing/non-directory roots exit 2. Relative root paths use cwd. Request resolution
uses root/identifier by default (without prefix); relative Lua source overrides
also use root. Only absolute Lua source overrides work without a root. See [source resolution](source-resolution.md).

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

See the [complete option table](#complete-option-table) for names, defaults and ranges.

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

See the [complete option table](#complete-option-table) for names, defaults and ranges.

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
without a bucket. One S3 backend/bucket can be configured per server instance.

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

See the [complete option table](#complete-option-table) for names, defaults and ranges.

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

Startup initializes, recovers and locks the store. JPEG/PNG/WebP image GET/HEAD requests
reuse complete encoded results after current preflight, source/version, header,
geometry and capability checks. Hits skip pixel decoding/processing/encoding;
unchanged headers are reused by the independent [metadata cache](metadata-cache.md).
Its defaults are 256 entries / 64 MiB accounted resident storage;
these are independent of the persistent cache limits. Storage failures fall back
to uncached delivery.
See [HTTP cache integration](persistent-cache.md#http-integration).


## Metadata cache limits

See the [complete option table](#complete-option-table) for names, defaults and ranges.

Both settings independently follow **CLI > environment > TOML > default**.
Use unsigned decimal integers, from zero to the platform's `size_t` maximum
(`18446744073709551615` on supported 64-bit builds). No signs, units, fractions or
empty values. Overflow, duplicates, missing arguments and unknown keys are errors.
Either setting at **0 disables metadata retention**; header reading and delivery
continue normally. Limits are resolved even when the persistent file cache is off.
Help/version validate explicit CLI values but skip environment and file loading.

```toml
metadata-cache-max-entries = 256
metadata-cache-max-bytes = 67108864
```

The limits bound resident entry count and accounted metadata bytes (including ICC
profiles and directory/component vector capacities). Allocator overhead and
caller-retained evicted snapshots are additional. See [metadata cache](metadata-cache.md).
`MetadataCacheLimits` is the shared source of default values; startup passes the
resolved value through the server into the shared worker pool. A limit does not eagerly
allocate that amount of memory. Tests verify precedence and runtime zero/one-entry
behavior, including environment wiring and CLI re-enablement.

## Watermark mask cache limits

See the [complete option table](#complete-option-table) for names, defaults and ranges.

Both follow **CLI > environment > TOML > default**, independently of metadata and
persistent result caches. The unsigned-decimal validation, help/version and zero
rules match the metadata cache above: either zero disables retention, not delivery.
`MaskCacheLimits` is the single source of defaults. Limits apply pool-wide to
resident immutable coverage masks and accounted storage; allocator overhead and
active requests retaining evicted masks are additional. They do not reserve memory.
Every request still opens/version-checks the mask source. See
[mask cache behavior](watermarks.md#decoded-mask-memory-cache).

```toml
mask-cache-max-entries = 64
mask-cache-max-bytes = 33554432
```

## Reduced public raster cache limits

See the [complete option table](#complete-option-table) for names, defaults and ranges.

Both follow CLI > environment > TOML > default. Either zero disables retention,
not the public resolution restriction. `VariantCacheLimits` provides defaults.
The pool-wide LRU retains complete immutable reduced rasters, before watermarks.
Resident accounting excludes allocator/control-block overhead and active references
after eviction. There is no eager allocation or persistence of these rasters;
the existing encoded result cache is unchanged. See
[restricted public resolution](restricted-resolution.md).

## IIIF worker concurrency

See the [complete option table](#complete-option-table) for names, defaults and ranges.

Example: `--iiif-workers 2` or `iiif-workers = 2` in TOML. Priority remains
CLI > environment > TOML > default; zero is invalid. One selects serial processing.
This setting is independent of Drogon's I/O thread and metadata-cache capacity.
See [worker concurrency](worker-pool.md) for memory planning and cache coordination.

## Bitonal rendering

See the [complete option table](#complete-option-table) for names, defaults and ranges.

Allowed case-sensitive values are `threshold` and `floyd-steinberg`. Empty,
unknown or duplicate selected values fail startup. Normal CLI > environment >
TOML > default precedence applies; help/version skip environment/file loading.
The setting changes only `bitonal` requests, not their IIIF URL. The persistent
cache separates methods. See [color policy](color-management.md)
for the encoded-space algorithm, memory accounting and independent-tile limitation.


## Output limits

The three output settings are --max-width / SIPI_MAX_WIDTH / TOML max-width,
--max-height / SIPI_MAX_HEIGHT / TOML max-height, and
--max-area / SIPI_MAX_AREA / TOML max-area.
Defaults: width 8192, absent height inherits the resolved width, area 8388608.
Axes must be 1..32768 and area 1..8388608; zero/unlimited is not supported.
They use the usual per-field precedence and appear as maxWidth/maxHeight/maxArea
in info.json. See [output limits and resource admission](output-limits.md) for
the shared geometry policy, rotated canvas checks and technical budget boundaries.

## Browser origins and cookies

See the [complete option table](#complete-option-table) for names, defaults and ranges.

Normal per-field priority applies: CLI > environment > TOML > default.
Origins is a single comma-separated string of exact HTTP(S) origins, without
spaces or paths, or the exclusive wildcard. Credentials accepts true/false and
requires explicit origins. See [CORS contract and examples](iiif-http.md).

## Explicit Lua configuration and secrets

See the [complete option table](#complete-option-table) for names, defaults and ranges.

Lua sees strings in `sipi.config` and bytes in `sipi.secrets`, available already at
initialization. Defaults are empty maps. Names are lower-case identifiers; environment
suffixes normalize to lower case. Priority applies **per name**, including across
literal/file secret sources; other lower-priority entries survive. Same-source
name duplicates/collisions fail. Settings require a preflight script (except
help/version); selected files are read only at startup. Only explicitly prefixed
environment variables are exposed, never the whole process environment.

See [the full contract](lua-configuration.md) for examples, source/size limits,
help behavior, exact file bytes, secret handling, snapshot lifetime and diagnostics.

## Listener address

`--listen-address`, `SIPI_LISTEN_ADDRESS`, or TOML `listen-address` selects a numeric
IPv4/IPv6 bind address (no hostname, brackets, port or zone suffix). Default:
`127.0.0.1`. Precedence remains CLI > environment > TOML > default, independently
of `--port` / `SIPI_PORT` / TOML `port` (default 8080). Containers typically use
`0.0.0.0`; do not publish their port directly when Caddy is the intended ingress.
The bind address does not determine canonical URLs: configure `public-base-url`
separately. Docker's image default sets SIPI_LISTEN_ADDRESS=0.0.0.0.

## JPEG encoding

See the [complete option table](#complete-option-table) for names, defaults and ranges.

Quality must be an integer from 1 to 100. Sampling accepts exactly `4:4:4` or
`4:2:0`. Both resolve independently in CLI > environment > TOML > default order,
with the usual invalid-value and help/version rules. Settings apply server-wide
at startup; restart after changing them. They are not per-request IIIF parameters.

```toml
jpeg-quality = 85
jpeg-sampling = "4:2:0"
```

```sh
./build/Debug/sipi-2 --jpeg-quality=85 --jpeg-sampling=4:2:0
SIPI_JPEG_QUALITY=85 SIPI_JPEG_SAMPLING=4:2:0 ./build/Debug/sipi-2
```

4:4:4 retains full chroma resolution; 4:2:0 halves chroma resolution horizontally
and vertically while retaining full luminance resolution. It typically reduces
file size but can blur fine coloured edges/text. Quality controls quantization;
100 is still lossy, and the setting is not a direct speed control. Actual speed
and size depend on image content and encoding. Defaults preserve previous output.

Gray/bitonal JPEG has one component, so sampling is ignored; quality still applies.
PNG/WebP encoding and ICC handling are unchanged. The encoded-result cache binds
quality and effective sampling; switching settings cannot reuse incompatible JPEG
bytes. Original 4:4:4 keys remain valid, and switching back can reuse old entries.
Unused variants are removed by normal LRU cleanup; no manual purge is needed.
See [JPEG delivery](jpeg-delivery.md) and [cache identity](cache-key.md).

## Tests and evolution

`src/CommandLine.cpp` implements the CLI11 loader; `src/main.cpp` snapshots the
selected environment variables and initializes runtime services. Configuration
changes belong in the explicit `ServerConfig`/loader and corresponding tests.
Defaults for cache/output/worker/codec behavior come from their shared value types,
not an independent documentation registry.

Run the existing focused configuration suites after building:

```sh
ctest --test-dir build/Debug --output-on-failure \
  -R '^(startup-configuration|s3-configuration|script-configuration)$'
```

For HTTP startup and script-secret integration, see `script-config-http`,
`cache-startup` and the broader [test workflow](makefile.md). These checks are not
substitutes for a fresh-checkout build or for actual S3/deployment verification.
