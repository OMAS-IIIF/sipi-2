# Shared Lua value cache

`sipi.cache` shares explicitly cached values between fresh Lua evaluations of one
`LuaPreflight` policy/configuration snapshot. C++ contains no application-specific
keys, tokens, endpoints or permissions. Cache ownership follows that policy:
copies share it; a newly constructed policy, server restart or another server
process starts empty. Entries are never written to SQLite or the image cache.

## API

```lua
function preflight(request)
    local value, revision = sipi.cache.get_or_load("service-data", function()
        -- Only one concurrent caller runs this loader for the same missing key.
        local response = sipi.http.request {url = sipi.config.lookup_url}
        if not response or response.status ~= 200 then error("Lookup failed") end
        return sipi.json.decode(response.body), 30 -- value, TTL in integer seconds
    end)

    local copy, observed_revision = sipi.cache.get("service-data")
    -- copy is independent of value and the shared cache, including nested tables.
    -- When a downstream service rejects this particular generation:
    -- sipi.cache.invalidate("service-data", revision)
    return {decision = "allow"}
end
```

| Operation | Result |
| --- | --- |
| `get(key)` | `value, revision`, or `nil, nil` when absent, expired or loading; never waits |
| `get_or_load(key, loader)` | `value, revision`; runs loader on a miss or waits for an existing loader |
| `invalidate(key, revision)` | Boolean; removes only that exact currently stored generation |

A loader returns **exactly two values**: a non-nil JSON-compatible value and an
integer TTL from 0 through 3600 seconds. TTL zero shares the result with callers
already waiting for that load, but removes the entry before returning; subsequent
calls load again and `get` cannot retrieve it. Its revision cannot invalidate a
later value. This supports transient outcomes without retaining denials or errors. False and `sipi.json.null` are valid cached
values. Strings, numbers, booleans, nested arrays/objects and JSON null follow the
[JSON helper contract](lua-json.md). Functions, Lua threads, ordinary userdata,
cycles, unsupported metatables, invalid UTF-8 and ambiguous tables are rejected.
Each read reconstructs its own value; mutating a returned table does not update
the cache. There is no automatic interpretation of JWTs or permission results.

Revision numbers are opaque Lua integers, unique within a cache lifetime. Always
pass the revision obtained with the value that failed. An old failure must not
invalidate a newer replacement. In-flight loads cannot be invalidated; eviction
and expiry may make invalidation return false. A completed value already copied
by another request cannot be retracted.

## Bounds and concurrency

- At most 256 entries, including active loads, and 4 MiB total resident key plus
  serialized-value bytes per policy. Structural overhead is bounded by entry count;
  conversion buffers and copies held by active callers are additional bounded work.
- Keys: 1..256 bytes, no NUL. Values: at most 64 KiB of serialized JSON.
- TTL uses a monotonic clock and starts **before the loader runs**. This deliberately
  counts network/loading time against the lifetime; a value whose positive TTL elapsed
  during loading is rejected. Reads do not extend expiry.
- Expired entries are removed lazily on access. Capacity evicts least recently
  used completed entries; active loads are never evicted. If only active loads
  occupy the necessary capacity, the operation fails instead of growing.
- At most 64 cache operations per Lua evaluation. Waiting callers share an
  absolute deadline five seconds after their first `get_or_load`. Native HTTP
  retains its separate operation/time budgets. A waiting timeout does not cancel
  the owner; its successfully loaded value may serve later requests.
- One loader per missing key; unrelated keys can load concurrently. No mutex is
  held while running Lua or HTTP. A failed owner removes the flight and wakes its
  existing waiters with failure; later requests can retry. Errors are not cached.
- Nested `get_or_load` calls inside a loader are rejected, including different
  keys, to prevent dependency cycles across workers. Nonblocking `get` remains
  available inside a loader.

Operations are enabled only while `preflight(request)` runs. Top-level validation
and state teardown cannot read or change the cache. Native cache failures and Lua
loader failures become credential-free Lua errors; the server maps an uncaught
error to its usual technical preflight failure. No stale entry is served on error.
A successful cache fill is independent of the enclosing policy decision: a later
deny or error does not roll back the fill. Do not use the cache as a transaction.

The namespace is **shared across all users of the same policy**, not scoped to
an individual request. Scripts must include all relevant identity dimensions in
keys when storing user-specific data. The library supplies bounded value storage;
application policy determines whether a particular value is suitable for caching.

The current limits are library defaults, not new command-line settings. They can
be made configurable if a concrete workload needs different bounds.

## OLDAP login reuse

The example uses `oldap.anonymous-login` for the service login credential. Its
endpoint/user/settings are immutable for that policy snapshot. It requires the
login API's `expiresIn`, uses `min(600, expiresIn - 5)` seconds, and includes login
request time in that TTL. There is no decoding of unverified JWT expiry claims.
Successful anonymous permissions now have a separate, configurable 600-second
cache per image. See [the policy contract](oldap-preflight.md#anonymous-permission-cache).
Login renewal occurs outside the permission loader; a lookup 401 uses a zero-TTL
outcome before retrying, so neither cache requires nested loads.

On lookup HTTP 401, the script invalidates only the revision it used, reloads
through the same coalesced cache operation and retries the lookup once. Repeated
401 denies access; 403/404 do not refresh the login. Invalid incoming media tokens
still deny without anonymous fallback. Worst case remains four HTTP calls, within
the existing helper budget. Restart forgets all login tokens.

Implementation: `src/preflight/SharedCache.*` (thread-safe owning cache),
`LuaCache.*` (protected Lua conversion and per-evaluation guards), and policy-owned
cache lifetime in `LuaPreflight.*`. Native concurrency/expiry/LRU/failure tests and
Lua isolation/allocation tests live in `tests/preflight/SharedCacheTests.cpp`;
`tests/oldap_preflight_http.py` checks real multi-request login counts, permission
checks, expiry and bounded refresh against a local fixture API.
