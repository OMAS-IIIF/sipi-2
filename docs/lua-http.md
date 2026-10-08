# HTTP(S) in preflight scripts

`sipi.http.request(options)` calls an external service synchronously from the
preflight worker. It is generic: endpoints, authentication, JSON interpretation
and access decisions belong to the script. No OLDAP-specific behavior is built in.
The native transport uses the already locked libcurl dependency from the AWS SDK;
no new package/version is introduced. It is independent of Drogon's event loop.

```lua
function preflight(request)
    local response, err = sipi.http.request {
        url = "https://permissions.example.org/check",
        method = "POST",
        headers = {
            ["Content-Type"] = "application/json",
            ["Accept"] = "application/json"
        },
        body = sipi.json.encode({identifier = request.identifier}),
        timeout_ms = 2000,
        max_response_bytes = 262144
    }
    if not response then
        error(err.message) -- Technical failure; do not accidentally grant access.
    end
    if response.status == 403 then
        return {decision = "deny"}
    end
    if response.status ~= 200 then
        error("Unexpected permissions-service status")
    end
    local data = sipi.json.decode(response.body)
    return {decision = data.allowed == true and "allow" or "deny"}
end
```

This illustrates a contract, not a prescribed authentication scheme. Explicit
`Authorization` or `Cookie` headers can be supplied by the script. Incoming browser
credentials are **never automatically forwarded** to the remote service.

## Request options

Pass exactly one plain table (no metatable); unknown options are errors.

| Field | Meaning/default | Bound |
| --- | --- | --- |
| `url` | Required absolute `http://` or `https://` URL | 8 KiB, no userinfo, fragment, spaces or control characters |
| `method` | `GET` | Uppercase GET, HEAD, POST, PUT, PATCH, DELETE, OPTIONS |
| `headers` | Optional plain table, name → string or nonempty dense array of strings | 128 field values, 64 KiB combined |
| `body` | Optional binary-safe string | 1 MiB; forbidden for HEAD |
| `timeout_ms` | 5000; whole transfer, including connect/read | Integer 1–10000 |
| `connect_timeout_ms` | 2000, additionally capped by transfer timeout | Integer 1–2000 |
| `max_response_bytes` | 1048576 | Integer 1–1048576 |
| `ca_file` | Optional trusted operator-provided CA bundle path | Nonempty, at most 4096 bytes, no NUL |

Limits are implementation constants; there are no new CLI/environment/TOML fields
in this step. Script settings can lower limits or choose a timeout up to the stated
ceiling. Deployment-specific options/credentials can come from
[explicit script configuration](lua-configuration.md).

Header names are case-insensitive; two spellings of the same name in the options
table are rejected. Empty string sends an empty field. Multiple values produce
multiple fields. Control characters/CRLF injection are rejected. The transport
owns Host, Content-Length, Transfer-Encoding, Connection, Expect, Upgrade, TE,
Trailer and Proxy-* fields, so scripts cannot override them. It computes body
length and uses the URL as the authority. Set Content-Type explicitly for JSON;
JSON encoding/decoding is always separate from the HTTP function.

## Response and failure contract

Success returns `response, nil`:

```lua
{
    status = 200,
    headers = { ["content-type"] = {"application/json"},
                ["set-cookie"] = {"a=1", "b=2"} },
    trailers = {},
    body = "..."
}
```

All response header names are lowercase and **all values are arrays**, including
single values. Repeated values retain arrival order within their field name;
global field ordering is not retained. Interim response fields are discarded;
only the final response headers are exposed. Trailers are separate so they cannot
silently replace an authorization-relevant initial header. The header byte/count
budget includes interim responses and trailers. No automatic content decompression
is requested; chunk framing is removed by libcurl, but a Content-Encoding such as
gzip remains encoded in the body. HEAD returns an empty body.

Every completed HTTP response, including 3xx/4xx/5xx, is a success at the transport
level. The script decides what each status means. There are no automatic redirects
or retries and no cookie jar. In particular, a redirect cannot automatically send
credentials to another destination.

Transport failure returns `nil, {code=..., message=...}` with no partial response:

- `timeout`: transfer deadline exceeded.
- `tls`: certificate verification or TLS handshake failure.
- `network`: connection, DNS or other transfer failure, including truncated bodies.
- `response_limit`: response body or header budget exceeded.
- `protocol`: malformed response headers or invalid final HTTP status.
- `budget`: this preflight evaluation exhausted its call/time budget.
- `memory`: a response callback could not allocate storage.

Messages contain no URL, credentials, request body or remote response fragments.
Invalid arguments, unavailable transport facilities and native setup failures
raise a Lua error instead; `pcall` can catch it. Uncaught Lua errors retain the
existing preflight failure behavior. No error is an implicit authorization grant.
The caller is responsible for interpreting `nil, err`; checking only HTTP status
without checking the response first will correctly fail the script.

## TLS, lifetime and resource policy

Certificate-chain and hostname verification are always enabled; there is no
`verify=false` escape hatch. Without `ca_file`, libcurl uses its configured default
CA sources, which depend on the package/platform. Deployments must provide a
usable trust store; `ca_file` can explicitly select a private/public CA bundle.
Keep that path under operator control, not incoming request control.

Only HTTP(S) is enabled. Ambient proxy environment variables and `.netrc` are not
used. Private-network/loopback services are deliberately allowed for deployments
such as local permission APIs. Scripts are trusted operator code: they should
choose approved endpoints instead of accepting arbitrary user-provided URLs.
This facility is not an SSRF sandbox or a general untrusted-code execution service.

Each evaluation owns one lazily created HTTP session. Its connection cache can be
reused by up to **four requests** in that evaluation, with a **ten-second aggregate
deadline measured from the first request**. Individual transfer timeouts are
clamped to the remaining time. Failed transfers count too; argument validation
errors do not. Requests block a worker, never the Drogon I/O thread. libcurl must
provide asynchronous DNS so DNS resolution can observe the transfer timeout;
a build without that capability fails explicitly.

Lua states/sessions remain isolated between evaluations and threads. There is no
cross-request connection pool, token cache, authorization cache or shared cookie
state. Source/image cache hits still require preflight authorization. Both request
and response buffers are native C++ storage outside the Lua 16 MiB allocator;
conversion into Lua briefly holds both copies. Native work is bounded separately,
not by Lua's instruction hook; deadlines do not preempt arbitrary local allocation
or operator-configured CA-file I/O.

Network access is disabled during top-level initialization, including startup
`validate()` and repeated script loading for evaluation. Calls belong inside
`preflight(request)`. It is disabled again before result-contract processing and
Lua-state teardown, including after errors, so finalizers cannot initiate requests
outside evaluation. HTTP callbacks never call Lua or let C++ exceptions escape.
Response conversion occurs beneath a protected Lua call and destroys native owners
before propagating allocation errors.

## Files, tests and next step

- `src/preflight/HttpClient.*`: owning request/response values, per-evaluation
  session, bounded libcurl transport and header handling.
- `src/preflight/LuaHttp.*`: strict Lua argument/result conversion.
- `tests/preflight/lua_http.py` and `LuaHttpTests.cpp`: offline loopback HTTP/TLS
  fixtures with a generated short-lived certificate; requires Python and the
  `openssl` CLI. No external service or permanent certificate is used.

```sh
cmake --build --preset conan-debug
ctest --preset conan-debug -R '^lua-(http|preflight)$' --output-on-failure
```

Tests cover methods, JSON/binary bodies, header multiplicity, connection reuse,
credential isolation, parallel evaluations, redirects, informational responses,
trailers, truncated responses, limits/deadlines, TLS trust/hostname rejection,
invalid options, startup prohibition and Lua allocation-failure recovery.

Generic [JWT verification/signing](lua-jwt.md) now provides explicit algorithm/key
and claim-validation semantics. [Script configuration/secrets](lua-configuration.md) are also available;
request-context additions are the next integration step.

Reference contracts: [libcurl timeouts](https://curl.se/libcurl/c/CURLOPT_TIMEOUT_MS.html),
[threading/DNS constraints](https://curl.se/libcurl/c/threadsafe.html) and
[protocol restriction](https://curl.se/libcurl/c/CURLOPT_PROTOCOLS_STR.html).
