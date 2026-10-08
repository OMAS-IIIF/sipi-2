# Lua preflight contract

The standalone `preflight` CMake target evaluates trusted operator-written Lua
policies. It has no Drogon or codec dependency. The HTTP adapter invokes it for
IIIF requests on a dedicated worker after startup validation. Lua 5.4.8 is pinned through Conan; no system Lua installation is required.
The initial implementation uses Lua's C API directly, without a binding framework.

## Files and use

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
C++ diagnostics can include script-supplied secrets. The server prints only the
error category at startup/runtime and never uses raw diagnostics as client bodies.

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

## Validation and next steps

```sh
conan install . -pr:h profiles/macos-debug -pr:b sipi-2-local --build=missing
cmake --preset conan-debug
cmake --build --preset conan-debug
ctest --preset conan-debug -R lua-preflight --output-on-failure
```

Tests exercise input transport, missing/empty headers, UTF-8 and binary strings,
owning outputs, invalid fields/types, missing functions, syntax and runtime errors,
non-string errors, successful calls after errors, snapshots, and concurrent calls.
The module also passes an ASan/UBSan build with strict compiler warnings; Conan
dependency binaries are not sanitizer-instrumented.

HTTP integration uses a bounded worker pool with 64 waiting jobs and the status
mapping in [HTTP routes](http-server.md). Script-path configuration and startup
validation are described in [startup configuration](command-line.md). Add reduced-source variants in a separate understandable increment.
[Explicit script configuration/secrets](lua-configuration.md) are available. Generic [JWT signing/verification](lua-jwt.md) is available. Outbound HTTP(S) is now
available through [sipi.http.request](lua-http.md).
The [delivery architecture](iiif-delivery-architecture.md) remains the guide for
the eventual processing order and authorization/cache boundaries.

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
Generic [JWT signing and verification](lua-jwt.md) are also available. Configured values are available in `sipi.config`/`sipi.secrets`; request-context
extensions are the next step.

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
