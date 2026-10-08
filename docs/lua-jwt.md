# JWT signing and verification in Lua

`sipi.jwt.sign(claims, options)` creates a signed compact JWT.
`sipi.jwt.verify(token, options)` returns verified claims or `nil, error`.
Both are generic helpers; permission values, resource IDs, repository paths and
other application-specific claim meanings belong to the preflight script.

The implementation uses pinned `jwt-cpp/0.7.2` with the existing OpenSSL and
JsonCpp dependencies. Signatures and compact-token construction/verification use
jwt-cpp; strict JSON/base64url validation and explicit claim policy supplement it.
No unverified-decode function, remote-key loading, JWKS discovery, token storage,
revocation list or authorization cache is provided in this step.

## Verification

```lua
-- verification_key must come from a trusted operator-controlled source.
-- Pass sipi.secrets.jwt_key (see lua-configuration.md).
local function verify_access_token(token, verification_key)
    local claims, err = sipi.jwt.verify(token, {
        algorithm = "HS256",
        key = verification_key,
        issuer = "https://issuer.example.org",
        audience = "image-service",
        type = "JWT",
        leeway = 15
    })
    if not claims then
        return nil, err
    end
    return claims
end
```

`verify` accepts exactly two arguments: a token string and a plain options table.
Unknown options, metatables, coercible-but-wrong types, invalid keys and unsupported
algorithms raise Lua errors. Incoming malformed/expired/invalid-signature tokens
return `nil, {code=..., message=...}`. No unverified claims are returned. A valid
signature is not itself permission to deliver a resource: the script must bind
claims to the requested identifier/project and apply its access policy.

| Option | Meaning |
| --- | --- |
| `algorithm` | Required exact `HS256` or `RS256`; never taken from the token |
| `key` | Required raw HMAC secret or RSA public PEM, according to algorithm |
| `issuer` | Optional required exact `iss` string |
| `audience` | Optional required audience string; accepts matching `aud` string or membership in an array |
| `type` | Optional required exact JOSE header `typ`, for example `JWT` or `at+jwt` |
| `require_exp` | Boolean, default `true`; explicitly set false only for a protocol allowing nonexpiring tokens |
| `leeway` | Integer seconds, default 0, allowed 0–300; applies to exp/nbf/iat |

**A token containing `aud` requires an explicit audience option.** Omitting the
option does not silently ignore the recipient claim. If the option is set,
a missing or nonmatching `aud` fails. Configure issuer and audience for the
particular service; there is no project-specific default. A missing issuer option
does not check issuer identity, though an `iss` value must still have string type.

The current time is sampled from the host's system clock as UTC Unix seconds.
There is no script-supplied `now` option that could bypass expiry checks. With
leeway `L`, acceptance requires:

- `now < exp + L` when exp is present; equality is expired.
- `now >= nbf - L` when nbf is present.
- `now >= iat - L` when iat is present (future issuance is rejected).

Arithmetic uses int64 seconds without converting token dates to chrono durations,
so large timestamps cannot overflow the platform clock representation. Set an
accurate host clock. `require_exp=false` permits absence only: existing exp/nbf/iat
are always checked, and malformed date claims are always rejected.

Supported NumericDates are **nonnegative signed-64-bit integer seconds**.
Fractional, exponent-decoded floating-point, string, boolean and null date claims
are rejected. This is an explicit supported subset of JWT NumericDate, which can
also represent fractions. `iss`, `sub` and `jti`, when present, must be strings;
`aud` must be a nonempty string or nonempty array of nonempty strings. Other claims
retain the normal [JSON mapping](lua-json.md), including nested structures, null,
booleans and 64-bit integers. Header `typ` and payload `typ` are separate: a
project-specific payload type such as `media` remains a script-level check.

Failure codes are `malformed`, `algorithm`, `signature`, `claims`, `expired` and
`not_yet_valid`. Messages contain no token, key or claim fragments. They are not
HTTP response bodies: the preflight script decides whether to refuse access or
follow an explicitly designed anonymous-access policy. Configuration/programming
errors must not be mistaken for an ordinary token refusal.

## Signing

```lua
local function issue_access_token(subject, signing_key)
    return sipi.jwt.sign({
        iss = "https://issuer.example.org",
        aud = "image-service",
        sub = subject,
        permissions = {read = true}
    }, {
        algorithm = "HS256",
        key = signing_key,
        expires_in = 300,
        kid = "current-key"
    })
end
```

Claims must encode as a JSON object. The second argument is a plain options table:

| Option | Meaning |
| --- | --- |
| `algorithm` | Required `HS256` or `RS256` |
| `key` | Required HMAC secret or unencrypted RSA private PEM |
| `expires_in` | Optional integer lifetime, 1–31536000 seconds, relative to the host clock |
| `type` | JOSE header typ, default `JWT` |
| `kid` | Optional header key identifier; descriptive only, never a file/URL lookup |

Provide either `claims.exp` or `expires_in`, never both. An expiration is always
required for newly signed tokens. `expires_in` sets exp and supplies iat if absent;
with explicit exp, no time claims are inserted. Other registered claims belong in
the claims object, not the signing options. Supplied registered claim types are
validated. Explicit dates are preserved, including past dates; the signer does
not promise that the result is currently acceptable to a verifier.

Signing returns a token string or raises a Lua error. No `alg` header override,
`none` mode, custom critical extensions or automatic key choice is accepted.

## Keys, token syntax and resource limits

- HS256 takes raw secret bytes, at least 32 bytes. Use a cryptographically random
  secret with sufficient entropy; length alone cannot guarantee that. A base64
  textual secret is treated literally, not automatically decoded. PEM material
  is rejected as an HMAC secret to prevent a common key-type confusion.
- RS256 signing takes an unencrypted private PEM; verification requires a public
  PEM (`BEGIN PUBLIC KEY`). RSA keys must be 2048–8192 bits. Wrong key kinds and
  invalid/encrypted PEM fail without password prompts. There is no key-file I/O.
- Keys are bounded to 16 KiB. Issuer/audience/type/kid options contain 1–1024 bytes
  without NUL. Protect keys at the deployment boundary and do not log tokens or
  keys. Lua/C++ string storage does not promise secure zeroization.
- Compact tokens are limited to 16 KiB, with exactly three nonempty canonical
  unpadded base64url segments. Duplicate JSON names, invalid UTF-8, ambiguous
  numeric spelling and noncanonical base64 padding/bits fail explicitly.
- Signing claims are limited to 10,000 serialized JSON bytes; final tokens still
  must fit 16 KiB. Existing JSON depth/node/string limits apply as well.
- Header extensions `crit`, `b64`, `jku`, `jwk`, `x5u`, `x5c`, `zip` and `enc` are
  rejected. Other noncritical headers are not interpreted; `kid` is never used
  automatically to select a key. Encrypted/nested JWT/JWE and detached payloads
  are not supported.
- A shared per-state budget allows 32 sign/verify attempts, including failures
  and top-level initialization. Exhaustion raises a Lua error and is not reset
  by `pcall`. New evaluations have independent budgets and keys/state.

JWT helpers are usable during initialization because they have no network or file
side effects. Native cryptographic work is synchronous on the preflight worker;
key/token size and operation budgets bound it separately from Lua instructions.
Native buffers lie outside Lua's 16 MiB accounting. Native objects are destroyed
before propagating Lua errors, including result-conversion allocation failures.

## Implementation and validation

`src/preflight/Jwt.*` implements the owning native contract with an injected clock
for tests; `LuaJwt.*` supplies the Lua boundary and real clock. `LuaJson.*` now shares
strict native parsing/serialization and protected Lua conversion between JSON and
JWT, preserving one value-mapping contract. A small jwt-cpp JsonCpp trait adaptation
preserves 64-bit integers instead of upstream's 32-bit `integer_type`. Explicit
second-based time checks enforce the exact expiration boundary and avoid chrono
overflow; signature verification stays in jwt-cpp/OpenSSL.

`tests/preflight/jwt_check.py` creates independent Python HMAC and OpenSSL RSA
vectors, and verifies SIPI-produced signatures independently. `JwtTests.cpp`
checks exact clock/leeway boundaries, dates after 2038, nested/null/64-bit claims,
algorithm/key confusion, malformed tokens, option failures, concurrent Lua states,
operation limits and Lua memory-failure recovery. Test keys live only in a temporary
directory. Python and the OpenSSL CLI are required; no external service is used.

```sh
conan install . -pr:h profiles/macos-debug -pr:b sipi-2-local --build=missing
cmake --preset conan-debug
cmake --build --preset conan-debug
ctest --preset conan-debug -R '^lua-(jwt|http|preflight)$' --output-on-failure
```

Explicit [script configuration and secrets](lua-configuration.md) now use
CLI > environment > TOML > defaults per name. Next extend the request context
(e.g. query parameters) and compose a deployment-specific example using the generic
JSON, HTTP and JWT helpers. Key rotation/JWKS and shared token caching remain
separate decisions, not implicit behavior of verify().

References: [jwt-cpp](https://github.com/Thalhammer/jwt-cpp),
[JWT claims (RFC 7519)](https://www.rfc-editor.org/rfc/rfc7519.html), and
[JWT best current practices (RFC 8725)](https://www.rfc-editor.org/rfc/rfc8725.html).
