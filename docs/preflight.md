# Lua preflight: access policies and source mapping

A Lua `preflight(request)` function decides whether an IIIF resource may be read,
which source to use, and whether to apply reduced public resolution or a watermark.
It runs before source access and encoded-result cache lookup for image, information
and base-URI requests, including HEAD. Without a configured script, the default
repository mapping is accessible without application authorization.

The script returns one decision table. SIPI performs storage access, image
processing and HTTP delivery. Lua does not receive a live connection or write the
response. Policies are trusted operator code; Lua 5.4.8 is bundled through Conan,
so no separate Lua installation is required.

## Quickstart: one public image alias

From the repository root, after the [native build](building.md):

```sh
./build/Debug/sipi-2 --config examples/native/sipi.toml \
  --preflight-script examples/preflight.lua
```

Stop any other demo using port 18080 first. The [minimal policy](../examples/preflight.lua)
allows only an empty prefix and identifier `example`, returning
`{ decision = "allow", source = "sample.png" }`. The source is relative to
`iiif-root`, which this configuration sets to `examples/images`.

In another terminal:

```sh
curl --fail http://127.0.0.1:18080/example/info.json
curl --fail http://127.0.0.1:18080/example/full/160,/0/default.jpg -o preview.jpg
curl --head http://127.0.0.1:18080/example/info.json
curl --include http://127.0.0.1:18080/example
curl --include http://127.0.0.1:18080/sample.png/info.json
curl --include http://127.0.0.1:18080/other/example/info.json
```

Expect 200 for info/image/HEAD, 303 from `/example` to its `info.json`, and 403 for
both the direct filename and the unapproved prefix. The preview is 160 × 100.
The alias illustrates source selection; it is a public allowlist, not a login.
Stop with Ctrl-C. Script edits require a restart.

Enable your own policy with `--preflight-script PATH`, `SIPI_PREFLIGHT_SCRIPT`, or
TOML `preflight-script = "PATH"`; normal [configuration precedence](command-line.md#sources-and-precedence)
applies. The script path uses the process working directory. Relative **returned
sources** instead use `iiif-root`. For container paths and mounted policies, see
[the Docker example](docker.md#toml-lua-and-a-persistent-cache).

## Request and decision flow

1. SIPI validates the IIIF path and query and submits a job to the worker pool.
2. A fresh Lua state initializes the snapshotted script and calls `preflight(request)`.
3. `deny` stops delivery with 403 before source access. A script/contract failure
   produces 500; it never grants access.
4. `allow` resolves the selected image and any required mask, checks metadata and
   applies the policy. Only then may an authorized request reuse an encoded result.
5. SIPI returns the image, information document, or base-URI redirect.

Health/hello and CORS OPTIONS do not invoke this policy. All IIIF request kinds
need authorization: allowing `info` or `base` also exposes resource information.
A cache hit does not replace the next request's policy check. Policy-dependent
responses use `Cache-Control: no-store`.

## Input at a glance

| Field | Lua value | Meaning |
| --- | --- | --- |
| `request.prefix` | Dense one-based array of strings; possibly empty | Decoded IIIF prefix segments; not automatically part of a storage path |
| `request.identifier` | String | Decoded resource identifier; no extension is added |
| `request.headers` | Table | Only optional `authorization` and `cookie`, not arbitrary HTTP headers |
| `request.cookies` | String-to-string table | Parsed cookies; use this for HTTP Cookie input |
| `request.query` | String-to-array table | Decoded query names with ordered value arrays, even for a single value |
| `request.kind` | `"image"`, `"info"`, `"base"` | Parsed route category |
| `request.method` | `"GET"`, `"HEAD"` | Original request method |

Tables are present even when empty; missing individual headers/cookies/query names
are `nil`. There are no region, size, rotation, quality, format, client-IP or arbitrary
header fields in this contract. Image-output limits belong to server configuration;
public-detail restrictions use `resolution`. Do not decode identifiers or query
values again. Detailed encoding/duplicate rules follow under [Lua input](#lua-input).

## Output at a glance

Return **exactly one plain table**. All nested policy tables must also have no metatable.

| Field | On allow | On deny |
| --- | --- | --- |
| `decision` | Required exact string `"allow"` | Required exact string `"deny"` |
| `source` | Optional local path string or `{type="s3", key="..."}` | Must be absent/nil |
| `resolution` | Optional `{max_width=1000, max_height=1000}`; both positive uint32 integers | Must be absent/nil |
| `watermark` | Optional required-mask policy described below | Must be absent/nil |

Unknown keys, coercible wrong types, missing decisions and multiple return values
are contract failures. `nil`, `false`, a string, or `{decision="deny", status=401}`
are not valid decisions. Lua cannot set a custom status, response body, redirect or
response header through this table. Denial maps to 403, not an authentication challenge.

| Allowed source | Storage behavior |
| --- | --- |
| Omitted | Identifier in the configured local root or S3 bucket; prefix ignored |
| `"sample.png"` | Local file relative to `iiif-root`, even with an S3 default |
| `"/srv/images/sample.tif"` | Trusted explicit absolute local path; may be outside the root |
| `{type="s3", key="project/sample.tif"}` | Literal key in the configured bucket, even with a local default |

Scripts cannot supply S3 endpoints, buckets or credentials. Prefer explicit
allowlists or validated project mapping over building absolute paths from request
strings. Relative/default local sources remain subject to root containment.
See [source resolution](source-resolution.md) for symlinks and storage failures.

## Example: JWT-protected image

The [complete JWT policy](../examples/preflight-jwt.lua) accepts an Authorization
Bearer token only, restricts requests to `/sample.png` with no prefix, verifies
HS256 signature/expiry/issuer/audience/type, and requires `sub = "sample.png"` plus
`read = true`. These last two claims are an example application contract, not
built-in SIPI permission names. Missing or rejected tokens return deny; helper misuse or quota failures remain
technical errors. Query/cookie
credentials are deliberately not alternate login paths in this example.

Set up a **local demonstration** with Python 3 and a temporary secret file:

```sh
demo_dir=$(mktemp -d)
echo "Demo directory: $demo_dir"
python3 - "$demo_dir" <<'PYTOKEN'
import base64, hashlib, hmac, json, secrets, sys, time
from pathlib import Path
root = Path(sys.argv[1])
key = secrets.token_bytes(32)
(root / "jwt.key").write_bytes(key)
(root / "jwt.key").chmod(0o600)
def encode(value):
    return base64.urlsafe_b64encode(value).rstrip(b"=")
def part(value):
    return encode(json.dumps(value, separators=(",", ":")).encode())
message = part({"alg": "HS256", "typ": "JWT"}) + b"." + part({
    "iss": "sipi-demo", "aud": "sipi-demo-images", "sub": "sample.png",
    "read": True, "exp": int(time.time()) + 300
})
token = message + b"." + encode(hmac.new(key, message, hashlib.sha256).digest())
(root / "token").write_bytes(token)
(root / "token").chmod(0o600)
PYTOKEN
./build/Debug/sipi-2 --config examples/native/sipi.toml \
  --preflight-script examples/preflight-jwt.lua \
  --preflight-config jwt_issuer=sipi-demo \
  --preflight-config jwt_audience=sipi-demo-images \
  --preflight-secret-file "jwt_key=$demo_dir/jwt.key"
```

Stop the previous demo first. In a second terminal, set `demo_dir` to the path
printed above:

```sh
curl --include http://127.0.0.1:18080/sample.png/info.json
curl --fail -H "Authorization: Bearer $(cat "$demo_dir/token")" \
  http://127.0.0.1:18080/sample.png/info.json
curl --fail -H "Authorization: Bearer $(cat "$demo_dir/token")" \
  http://127.0.0.1:18080/sample.png/full/160,/0/default.jpg -o preview.jpg
```

Expect 403 without a token and 200 with the generated token for five minutes.
Stop the server, then remove the temporary demo directory with `rm -r "$demo_dir"`.
For real deployments, use the application's trusted issuer and key-management
workflow; serve credentials over HTTPS and keep them out of logs. A client/viewer
must attach credentials to every info/image request. SIPI does not issue login
sessions or automatically forward a browser's token to subsequent requests.
See [JWT semantics](lua-jwt.md) for RS256 and claim validation, and
[configuration/secrets](lua-configuration.md) for TOML, environment and secret-file
snapshots. Do not wrap all verification/configuration failures in a blanket
`pcall` that silently allows access.

## More policy examples

- [Reduced public resolution](../examples/restricted-resolution.lua) allows only
  `/public/` with a fixed public coordinate system; the original remains denied.
  Set smaller limits than the source to observe reduction. This is a whole-image
  detail restriction, not the output-width limit of an individual IIIF response.
- [Watermark decisions](#single-color-watermark-decision) combine with source and
  resolution. Supply a supported grayscale mask as described in [watermarks](watermarks.md);
  a missing/invalid required mask fails delivery rather than serving an unmarked image.
- [Project-prefix mapping](#explicit-project-prefix-mapping) explicitly chooses
  S3 keys; a prefix alone is not proof of authorization.
- [OLDAP integration](oldap-preflight.md) is a separate deployment policy using
  JSON, HTTP, JWT, configuration and explicit shared caching.

## Lua input

```lua
request = {
    prefix = { "museum", "collection" },
    identifier = "image123",
    headers = {
        authorization = "Bearer ...",
        cookie = "session=..." -- Only when a caller has the original header.
    },
    cookies = { session = "..." },
    query = { token = { "..." }, tag = { "one", "two" } },
    kind = "image", -- "image", "info" or "base"
    method = "GET"  -- "GET" or "HEAD"
}
```

Prefix is a one-based array retaining decoded segment boundaries. Identifier and
prefix come from the IIIF parser; neither is implicitly a filesystem path. Do not
decode them again. Header keys are lowercase; missing headers are absent (`nil`),
while a supplied empty header is an empty string. Strings are copied with their
lengths. The caller remains responsible for HTTP validation and must not log
credentials. No live HTTP connection is exposed to Lua. request.cookies contains
parsed cookie values supplied by an adapter. Drogon removes the raw Cookie header,
so HTTP scripts must use this table rather than request.headers.cookie; the latter
remains optional for callers that can supply the original header.

### Query parameters and request context

`request.query` is always a table. Each case-sensitive decoded name maps to a
one-based array, including single values. Repeated names retain value order;
`?token=a&%74oken=b` becomes `query.token = {"a", "b"}`. Missing names are `nil`;
`?flag&empty=` supplies `{ "" }` for both. The HTTP adapter splits on `&` and the
first `=`, then percent-decodes exactly once; `+` means space, `%2B` means literal
plus. Semicolons are ordinary value characters. These rules apply only to queries;
IIIF path segments keep literal `+`. Do not decode query values again. Values are
byte strings without Unicode normalization.

Malformed percent escapes, empty names/components, ASCII controls (including NUL
and DEL) and raw fragment markers are HTTP 400. Limits are 32 KiB raw query,
128 name/value pairs (including duplicates), 256 decoded bytes per name and
16 KiB per value; exceeding them is HTTP 414. Validation precedes worker submission,
preflight and cache lookup, including when no script is configured. Diagnostics
contain no query values. These are application ceilings; proxies may impose lower
request-target limits.

`kind` describes the parsed IIIF route, never a query-supplied value:

- `image`: image request with region/size/rotation/quality/format.
- `info`: explicit `info.json` request.
- `base`: service base URI, authorized before redirecting to `info.json`.

`method` preserves the original GET/HEAD distinction even though Drogon internally
routes HEAD through GET. Both methods run preflight. OPTIONS is handled separately
by CORS and does not invoke the image policy. All fields are owning snapshots;
Lua changes are local to this evaluation and cannot alter the native request.

A policy using a query token should reject ambiguity before verifying it:

```lua
function preflight(request)
    local tokens = request.query.token
    if not tokens or #tokens ~= 1 or tokens[1] == "" then
        return { decision = "deny" }
    end
    local claims = sipi.jwt.verify(tokens[1], {
        algorithm = "HS256",
        key = sipi.secrets.jwt_key,
        issuer = sipi.config.jwt_issuer,
        audience = sipi.config.jwt_audience
    })
    -- This example's token contract binds sub to one image identifier.
    -- A deployment must also bind a prefix if it distinguishes different images.
    if not claims or claims.sub ~= request.identifier then
        return { decision = "deny" }
    end
    return { decision = "allow" }
end
```

Configure the key, issuer and audience explicitly as described in
[lua-configuration.md](lua-configuration.md). Token location, precedence versus
Authorization/cookies, resource binding and permission rules belong to the script;
there is no implicit token verification or OLDAP-specific policy in C++.

The base-URI 303 preserves the validated raw query on its configured-public-URL
`Location`, so a query token reaches `info.json`. Authorization runs again there.
Queries are excluded from information-document `id`, canonical links and cache
keys; the client must attach credentials to subsequent image requests itself.
Preflight still runs before every cache hit. Redirects remain `Cache-Control:
no-store`. The server does not log queries, but query credentials may appear in
browser history and proxy access logs; configure deployment logging accordingly.

## Lua output

Return exactly one plain table, with no metatable:

```lua
return { decision = "allow", source = "/srv/images/example.tif" }
-- or: return { decision = "allow" }
-- or: return { decision = "deny" }
```

`decision` must be exactly the string `allow` or `deny`. An allowed result can
contain `source`: either a nonempty local path string without NUL bytes, or
`{type="s3", key="literal-object-key"}`. The key must also be a nonempty string
without NUL bytes. The source table accepts only `type` and `key`, has no metatable,
and requires exactly `type="s3"`. Scripts cannot set buckets, endpoints or credentials. A denied result must not
contain a source. Unknown fields, numeric/string coercions, multiple return values,
and unsupported decisions are rejected. This catches misspelled or prematurely
introduced policy fields rather than silently ignoring restrictions.

`Allow` owns an optional `SourceOverride` (local path or `S3Key`); `Deny` is a separate value type.
No source on allow means the caller must apply an explicitly configured resource
mapping. The module does not open/check the source, resolve relative paths, enforce
repository boundaries, or treat an identifier as a path. Those responsibilities
belong to [source resolution](source-resolution.md): relative/default sources use
iiif-root, and explicit absolute script paths may bypass it. A string source
ALWAYS remains local, even with `iiif-storage="s3"`. An S3 table selects its key
in the configured bucket, even with `iiif-storage="local"`. The optional `resolution` field defines a reduced full public image; see [restricted resolution](restricted-resolution.md). The optional watermark contract
is defined below.

## Single-color watermark decision

An allow result may carry one centered, full-image-anchored watermark:

```lua
return {
    decision = "allow",
    watermark = {
        source = "masks/logo.png", -- or {type="s3", key="masks/logo.png"}
        width_fraction = 0.5,
        opacity = 0.25,
        color = {255, 255, 255} -- sRGB8, exactly three Lua integers
    }
}
```

All four watermark fields are required. `width_fraction` is a finite Lua number
in (0,1]; `opacity` is a finite number in [0,1]. Strings are not coerced to numbers.
`color` is a plain, dense array with exactly indices 1..3 and integer values 0..255;
extra keys, holes, floats and numeric strings fail. The watermark and nested source
must also be plain tables without metatables. Unknown fields and a watermark on a
`deny` result fail with a contract error. Nil/absent watermark means unrestricted
by this mechanism. Opacity zero is valid but does not bypass policy handling.

The source syntax is identical to the image override (nonempty local path or S3
key, no NUL). Image and mask sources are independent. No bucket, endpoint or
credential can be supplied in either source table. The evaluator only validates
and copies values into `Allow::watermark` (`preflight::Watermark`); it performs no
path resolution, storage access, decoding or color conversion. Source resolution
and mask-version binding belong to the delivery adapter, as for the image.

**Delivery:** The worker resolves and validates the mask before info/base responses
and encoded-result cache lookup, including HEAD. It applies the mark on the selected
decoder grid before crop/resize, mirror/rotation and quality conversion. Missing or
invalid required masks return `500 / watermark_failed`; an unmarked fallback is
never served. Mask source/version and every rendering parameter are part of result
cache identity. Responses remain `Cache-Control: no-store`. The mask source is opened/version-checked on every request; the bounded memory
cache reuses already validated samples. Misses decode within the 16 MiB encoded
and decoded limits. See the [mask cache](watermarks.md#decoded-mask-memory-cache).

See [watermark processing](watermarks.md) for the mask/color and placement rules.

## Lifetime and errors

Scripts, configuration and secrets are read at startup; restart to adopt edits.
Top-level code runs once for startup validation and again in each request's fresh
state. Keep it deterministic: define functions and validate local settings there.
Outbound HTTP and shared-cache operations are enabled only inside the request call,
not initialization or teardown. Startup checks syntax/initialization and the named
function; it does not execute a sample request or validate external services.

| Outcome | HTTP behavior |
| --- | --- |
| Explicit deny | 403 / `access_denied`; no image/mask source access |
| Lua exception, invalid return, instruction/allocation limit | 500 / `preflight_failed` |
| Allowed source missing | 404 / `image_not_found` |
| Required repository/backend configuration missing | 500 / `source_not_configured` |
| Relative source outside the allowed root | 403 / `source_not_allowed` |
| Required mask missing/invalid | 500 / `watermark_failed`; no unmarked fallback |

A missing/unreadable script, syntax error or failing top-level assertion prevents
server startup (exit 2). Client errors are generic; use operator logs for the
category and masked diagnostic. Never include credentials or backend bodies in
`error()`/`assert()` messages. Expected lack of permission should return deny;
technical backend failures should remain failures, not permission grants.

Each evaluation owns a fresh Lua state through `std::unique_ptr` with `lua_close`
as its deleter. Globals, modified library tables, stack values, and request data do
not survive a call unless explicitly stored through `sipi.cache`. Repeated and
concurrent const evaluations share only that synchronized cache; their Lua states
remain independent.
Script compilation and initialization therefore occur per call; measure that cost
before considering state reuse, which would require a deliberate isolation design.

All allocating Lua operations execute within one protected `lua_pcall` boundary.
Callbacks beneath it contain no nontrivial C++ automatic objects because C-built
Lua errors use longjmp. Owning C++ values are constructed after returning to the
host. Lua errors with non-string objects receive a fixed diagnostic instead of
invoking an error object's metamethod. Results are copied before state destruction.

`PreflightError` distinguishes script-file I/O, script loading, script execution,
invalid contracts, Lua allocation failures, and instruction exhaustion. C++ allocation failures may still
raise `std::bad_alloc`. A technical failure is never an authorization result.
Server logs include the category name/number and bounded, single-line Lua diagnostics,
including script path/line where Lua supplies them. Known secrets and incoming
credentials are masked; arbitrary script text can still be sensitive. See
[lua-configuration.md](lua-configuration.md#limits-and-diagnostics) for the exact
protection and script-author obligations. Client responses remain generic.

Available standard libraries are base, table, string, math, and utf8. Base functions
`dofile`, `loadfile`, `load`, `print`, and `warn` are removed. io, os, package, debug,
and coroutine libraries are not opened. Generic `sipi.json`, `sipi.http` and `sipi.jwt` host
functions are available as documented below.
This reduces incidental side effects but is not a sandbox. Each validation/evaluation
has a 16 MiB Lua allocator budget and one million VM instructions, including
initialization. Hook errors cannot reset the instruction budget via pcall. These
limits do not bound C++ allocations, file I/O, script text storage, native-library
execution time, or wall-clock duration. HTTP has separate transfer/aggregate
timeouts and byte limits; see [the HTTP contract](lua-http.md).

## Validation and tests

After building, run the focused existing contract and HTTP tests:

```sh
ctest --test-dir build/Debug --output-on-failure \
  -R '^(lua-preflight|preflight-context-http|script-config-http|lua-jwt)$'
```

These cover input/output types, invalid contracts, errors/quotas, isolated states,
query/context transport, GET/HEAD authorization before cache hits, configured
secrets and JWT checks. Also exercise the actual policy with allowed and denied
image/info/base requests; `/health` and startup alone do not call its request path.
The [build guide](building.md) describes the broader test workflow.

## Explicit project-prefix mapping

**The IIIF URL prefix stays outside the default mapping. Project-specific
translation belongs in the preflight script.** Without a source override,
`/museum/subpart/tiled.tif/info.json` selects local `<iiif-root>/tiled.tif` or
S3 key `tiled.tif`, depending on `iiif-storage`. The prefix remains available as
`request.prefix` and remains part of the public service identity.

A trusted policy can explicitly opt into project-dependent S3 key mapping:

```lua
function preflight(request)
    local parts = {}
    for i, segment in ipairs(request.prefix) do parts[i] = segment end
    parts[#parts + 1] = request.identifier
    return {
        decision = "allow",
        source = { type = "s3", key = table.concat(parts, "/") }
    }
end
```

With this script, the example selects `museum/subpart/tiled.tif`. This is an
operator-defined naming policy, not automatic IIIF behavior. Apply any required
project authorization in the same policy before returning allow. `deny` prevents
source lookup, including S3 HEAD/GET. Missing S3 configuration for an allowed S3
source produces `source_not_configured`; malformed Lua tables fail the contract.

## JSON helpers

`sipi.json.decode`, `encode`, `null`, `array()` and `object()` are available in
all script states. See [JSON mapping, limits and examples](lua-json.md).
Generic [HTTP(S) requests](lua-http.md) are available inside `preflight(request)`.
Generic [JWT signing and verification](lua-jwt.md) are also available. Configured values are available in `sipi.config`/`sipi.secrets`; query, route kind
and method are available in the request snapshot documented above.

## Explicit shared cache

Fresh Lua states remain isolated. Only values deliberately stored through
[`sipi.cache`](lua-cache.md) survive between requests of the same policy snapshot.
Cache operations are disabled at initialization/teardown; values are copied and
bounded. This is separate from both image caches and script configuration.

## Public resolution decision

`resolution = { max_width = 1000, max_height = 1000 }` on an allow result limits
whole-image detail before IIIF regions/sizes are interpreted. Both fields are
required positive uint32 Lua integers. Unknown fields/metatables/coercions and a
resolution on deny fail the protected contract. It combines with source and
watermark. Keep public/original service URLs distinct through Lua prefix policy;
never switch the coordinate system at the same URL on login. See
[restricted resolution](restricted-resolution.md) and the
[deny-by-default example](../examples/restricted-resolution.lua).

## Files and use

The standalone `preflight` CMake target uses the Lua C API directly and has no
Drogon or codec dependency. The HTTP adapter evaluates policies on image workers.

- `src/preflight/LuaPreflight.h`: owning request/decision types and public evaluator.
- `src/preflight/LuaPreflight.cpp`: script loading, Lua lifetime, protected calls,
  and strict result decoding.
- `examples/preflight.lua`: minimal allow/deny policy with one explicit source.
- `tests/preflight/LuaPreflightTests.cpp`: contract, error, and isolation tests.

```cpp
#include "preflight/LuaPreflight.h"

sipi::preflight::LuaPreflight policy("examples/preflight.lua");
sipi::preflight::Request request{
    .resource = {.prefix = {}, .identifier = "example"},
    .authorization = std::nullopt,
    .cookie = std::nullopt
};
policy.validate(); // Initialize and check the entry point without a request.
const auto decision = policy.evaluate(request);
// Inspect Allow or Deny; PreflightError represents a technical failure.
```

Construction snapshots the script text from disk and the explicitly supplied
`ScriptConfig` (empty by default). `sipi.config` and `sipi.secrets` are fresh tables
for each state, available during initialization; see [script configuration](lua-configuration.md). validate() compiles and runs
top-level initialization in a fresh state, then checks the global preflight function
without calling it. Evaluation compiles text-only
Lua, executes top-level initialization, and calls the global function
`preflight(request)`. Construct a new evaluator to adopt a changed script file.
Top-level chunk return values are ignored; this is a named-function contract,
not a module-return contract. A missing function fails explicitly.
