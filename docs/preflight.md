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

Construction snapshots the script text from disk. validate() compiles and runs
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
    cookies = { session = "..." }
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

## Lua output

Return exactly one plain table, with no metatable:

```lua
return { decision = "allow", source = "/srv/images/example.tif" }
-- or: return { decision = "allow" }
-- or: return { decision = "deny" }
```

`decision` must be exactly the string `allow` or `deny`. An allowed result can
contain `source`, a nonempty string without NUL bytes. A denied result must not
contain a source. Unknown fields, numeric/string coercions, multiple return values,
and unsupported decisions are rejected. This catches misspelled or prematurely
introduced policy fields rather than silently ignoring restrictions.

`Allow` owns an optional `std::filesystem::path`; `Deny` is a separate value type.
No source on allow means the caller must apply an explicitly configured resource
mapping. The module does not open/check the source, resolve relative paths, enforce
repository boundaries, or treat an identifier as a path. Those responsibilities
belong to [source resolution](source-resolution.md): relative/default sources use
iiif-root, and explicit absolute script sources may bypass it. Restriction/variant and watermark
fields are not implemented yet; adding them requires an explicit contract update.

## Lifetime and errors

Each evaluation owns a fresh Lua state through `std::unique_ptr` with `lua_close`
as its deleter. Globals, modified library tables, stack values, and request data do
not survive a call. Repeated and concurrent const evaluations are independent.
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
Diagnostics can include script-supplied text and are not client response bodies.

Available standard libraries are base, table, string, math, and utf8. Base functions
`dofile`, `loadfile`, `load`, `print`, and `warn` are removed. io, os, package, debug,
and coroutine libraries are not opened. There are no custom host callbacks yet.
This reduces incidental side effects but is not a sandbox. Each validation/evaluation
has a 16 MiB Lua allocator budget and one million VM instructions, including
initialization. Hook errors cannot reset the instruction budget via pcall. These
limits do not bound C++ allocations, file I/O, script text storage, native-library
execution time, or wall-clock duration. Native/network facilities will need their
own timeouts when added.

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

HTTP integration uses one bounded worker with 64 waiting jobs and the status
mapping in [HTTP routes](http-server.md). Script-path configuration and startup
validation are described in [startup configuration](command-line.md). Add reduced-source variants, JSON, JWT
verification/signing, and outbound HTTPS in separate understandable increments.
The [delivery architecture](iiif-delivery-architecture.md) remains the guide for
the eventual processing order and authorization/cache boundaries.
