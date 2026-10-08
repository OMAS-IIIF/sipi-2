# JSON in preflight scripts

`sipi.json` is available during script initialization and `preflight(request)`.
It has no HTTP, filesystem, environment or application-specific behavior. Each
Lua evaluation owns its values; there is no shared mutable JSON state.

```lua
local json = sipi.json

local data = json.decode('{"allowed":true,"roles":["reader"],"expires":null}')
assert(data.allowed)
assert(data.roles[1] == "reader")
assert(data.expires == json.null)

local body = json.encode({identifier = "image123", options = {preview = true}})
local empty_list = json.array()
local empty_object = json.object()
assert(json.encode(empty_list) == "[]")
assert(json.encode(empty_object) == "{}")
```

## Value mapping

| JSON | Lua |
| --- | --- |
| null | `sipi.json.null`, an identity sentinel; never `nil` |
| boolean | boolean |
| integer token | signed 64-bit Lua integer; overflow is rejected |
| fractional/exponent number | finite Lua number (double, usual floating-point rounding) |
| string | UTF-8 string; escaped NUL is supported |
| array | marked table with dense integer keys starting at 1 |
| object | marked table with string keys |

`decode(text)` accepts exactly one string, including scalar JSON documents.
Decoded arrays/objects retain their kind when emptied. `array()` and `object()`
accept no arguments and return fresh empty marked tables; populate them normally.
Their metatables are protected and do not implement indexing behavior.

`encode(value)` accepts exactly one value. Plain tables with string keys become
objects; plain dense sequences become arrays; a plain empty table becomes `{}`.
Mixed keys, holes, nonpositive indices, inappropriate keys in marked containers,
foreign metatables, functions, threads, arbitrary userdata and `nil` are errors.
Use `json.null` for null elements; assigning `nil` removes a table entry.
Cycles are rejected; repeated references to a noncyclic table are serialized as
independent copies. Object key order, whitespace and numeric spelling are not a
round-trip guarantee. This is not a canonical JSON/signature serializer.

Malformed UTF-8, malformed JSON, duplicate object keys, trailing documents,
comments, trailing commas, non-finite numbers and oversized integer tokens are
rejected. Decimal/exponent notation deliberately uses floating-point semantics;
use strings for identifiers outside signed 64-bit range or arbitrary precision.

## Errors and resource limits

Invalid input raises a Lua error. Scripts can catch it using `pcall`; an uncaught
error follows the existing `PreflightError::script_execution` path. Conversion
errors do not include JSON input fragments, which may contain secrets.

Each conversion has fixed limits:

- 256 KiB input JSON or encoded output; additionally, 256 KiB cumulative decoded
  string/key bytes on encode and decode.
- 16,384 value nodes, counting containers and scalar values.
- At most 64 nested edges from the root; the underlying parser also has a stack
  limit of 64, so deeply nested input can be rejected earlier.

Existing per-state Lua memory/instruction limits still apply. Native conversion
work is bounded separately by the limits above, not by the VM instruction hook.
Native JSON trees, decode operations and escaped output occupy additional memory
outside Lua's 16 MiB allocator budget; that budget is not a process-memory cap.
Output escaping can temporarily require more than the final accepted byte limit.
These limits are implementation constants, not new configuration options.

## Implementation and validation

`src/preflight/LuaJson.*` uses the JsonCpp dependency already present through
Drogon, linked directly to the independent `preflight` target. It introduces no
new Conan requirement or version change. Additional lexical checks enforce strict
number/control-character handling where JsonCpp accepts permissive spellings.

Lua can use `longjmp` for errors. Decode prepares a linear sequence of stack
operations with C++ ownership outside a protected `lua_pcall`. Encode traverses
Lua using raw, non-coercing reads and non-throwing stack checks, then creates the
result string inside a protected call. All C++ owners are destroyed before a
Lua error is propagated. No script metamethod executes during conversion.

`tests/preflight/LuaPreflightTests.cpp` covers startup, nested round trips,
Unicode/NUL, signed integer boundaries, null and empty containers, malformed
input, table shapes/cycles, resource limits, recovery and concurrent isolation.

Generic [HTTP(S) requests](lua-http.md) now provide methods, headers, body, response
status/headers/body, TLS verification and bounded network time/response size.
JSON conversion remains explicit, so HTTP is useful for non-JSON services too.
