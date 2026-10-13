# OLDAP deployment policy example

[The Lua policy](../examples/oldap/preflight.lua) composes the generic JSON, HTTP,
JWT and configuration helpers. It contains all OLDAP-specific claim meanings,
endpoints and repository layout; no C++ behavior was added for OLDAP. It is tested
against a local mock API and independently signed tokens, not a live OLDAP service.
No OLDAP repository or deployment is changed by installing this example.

## Configuration

Copy/adapt [sipi.toml](../examples/oldap/sipi.toml), then start sipi-2 with
`--config examples/oldap/sipi.toml` from the repository root. Configure the actual
repository root and secret mounts. All paths resolve from the process working
directory, including script and secret files. The local source root must contain
OLDAP's derived image hierarchy. Supported image formats still follow sipi-2's
normal input contract.

Script settings use the standard per-name CLI > environment > TOML priority:

| Script key | Kind | Meaning |
| --- | --- | --- |
| `media_jwt_key` | secret, required | Dedicated OLDAP HS256 media key, at least 32 bytes; never the access/refresh key |
| `jwt_issuer` | config | Default `https://oldap.org` |
| `jwt_audience` | config | Default `oldap-api-media` |
| `minimum_permission` | config, required | Integer string 1..6; explicit full-image access threshold |
| `api_url` | config, optional | OLDAP API base URL; absence disables anonymous lookup |
| `unknown_user` | config | Default `unknown` |
| `anonymous_permission_ttl` | config, optional | Successful anonymous grants: integer seconds 0..3600, default 600; 0 disables permission caching |
| `unknown_password` | secret | Required for a service account other than `unknown`; optional for the reserved anonymous pseudo-user |
| `ca_file` | config, optional | CA bundle for the API's TLS certificate; TLS verification remains enabled |
| `media_cookie` | config, optional | Name of a media capability cookie; absent disables this credential source |

For example, `SIPI_PREFLIGHT_SECRETS_MEDIA_JWT_KEY` supplies the signing key,
`SIPI_PREFLIGHT_CONFIG_API_URL` enables the fallback API, and
`SIPI_PREFLIGHT_SECRETS_UNKNOWN_PASSWORD` supplies the anonymous account password.
The reserved `unknown` pseudo-user is passwordless in the current OLDAP API. Leave
its password setting absent: the script sends `{"password":""}`. For a different
configured service account, supply a nonempty password explicitly. Secret files preserve exact bytes: an accidental final newline changes the key or
password. Restart to adopt changes. Use HTTPS for an API outside a trusted local
network; HTTP remains available for local testing.

`minimum_permission=1` reproduces the previous delegate's image rule. OLDAP names
permission 1 DATA_RESTRICTED and 2 DATA_VIEW; the distinction concerns resource
data and must not silently be interpreted as an image-resolution limit. Set 2 if
only full-view permission should grant image delivery. No watermark or virtual
reduced-image restriction is applied by this example. This choice is explicit,
mandatory, and covered by tests for both values.

## Credential selection and authorization

1. If `request.query.token` exists, require exactly one nonempty value.
2. Otherwise use the Authorization header, requiring a Bearer token.
3. Otherwise use the explicitly configured media cookie, when present.
4. Only when all those credentials are absent, use anonymous lookup if configured;
   otherwise deny.

A higher-priority credential wins. An empty, malformed, expired or unauthorized
credential denies access without falling through to lower-priority credentials or
the anonymous account. Access and refresh tokens are not accepted as media
capabilities. Cookies here carry the same media JWT as the query, not a login
session. The cookie table is supplied by Drogon; duplicate query values are
explicitly retained by sipi-2's query parser.

Verify HS256 with the dedicated key, issuer, audience and required expiry, then
require the **payload** `typ == "media"` (distinct from the JWT header type).
`id` must exactly equal the decoded requested identifier; when `assetId` exists,
it must agree too. `permval` must be an integer number in the configured threshold
through 6. Paths and filenames must be valid before granting access. All request
kinds (base/info/image) and GET/HEAD use the same policy. Prefixes are aliases here:
OLDAP asset IDs must be globally unique; a project namespace needs an explicit
additional binding policy.

The current OLDAP issuer in `oldaplib/src/authentication.py` emits integer `iat`
and `exp`, matching sipi-2's JWT contract. Legacy fractional/string NumericDates
are deliberately rejected, even though the old Ruby delegate coerced them.

## Anonymous REST lookup

On a login-cache miss, the anonymous path performs:

- `POST /admin/auth/<encoded-user>` with JSON `{"password":"..."}` and explicit
  Content-Type/Accept headers; require HTTP 200 and a nonempty `accessToken` string (`token` is accepted for older deployments).
- `GET /data/mediaobject/id/<encoded-id>` with that service token in Authorization
  and Accept JSON. Require HTTP 200; read numeric `permval`, `shared:path`, and
  `shared:derivativeName` from the object.

The identifier/user is encoded as one URL path segment. Incoming browser headers,
cookies and media tokens are never forwarded. Lookup 403/404 denies access; 401 permits one token refresh before denial;
login failures, other statuses, invalid JSON and transport failures produce a
technical preflight error (currently HTTP 500). Requests have 3-second transfer,
1-second connection and 64-KiB response limits within the generic HTTP budget.
No redirects or automatic retries are followed.

The login token is shared in memory through [sipi.cache](lua-cache.md), scoped to
the policy/configuration snapshot. Login must return a positive integer
`expiresIn` greater than five seconds; this is required even when using the legacy
`token` response field. The script caches for `min(600, expiresIn - 5)` seconds,
counting login time against the TTL. Parallel misses run only one login. On a permission-cache miss, the image gets
a fresh permission lookup before any rendered-image cache hit. Lookup
401 invalidates only the used cache revision and permits one refresh/retry;
403/404 deny without refresh. No cross-process or restart persistence.

## Anonymous permission cache

The Lua policy caches **only successful, fully validated anonymous decisions**:
`{decision="allow", source="<resolved relative path>"}`. It does not retain the API's
whole metadata object. Each key combines the anonymous-permission namespace with
the exact decoded image ID. API endpoint, service account, permission threshold
and credentials are immutable within the policy snapshot, which owns the cache.
Prefixes remain aliases; supplied media credentials always use JWT verification
and never fall back to an anonymous cached grant.

`anonymous_permission_ttl` defaults to **600 seconds** and accepts integers from
0 through 3600. Set 0 to disable permission reuse while keeping login-token reuse.
Use the existing generic configuration with its usual precedence:

```sh
sipi-2 --preflight-config=anonymous_permission_ttl=600
# Or: SIPI_PREFLIGHT_CONFIG_ANONYMOUS_PERMISSION_TTL=600
```

In TOML append `"anonymous_permission_ttl=600"` to `preflight-config`. For the
OLDAP Docker deployment pass the same generic environment variable to imageserver;
no extra OLDAP-specific native setting or environment adapter is needed.

- Expiry starts before the permission lookup. Hits do not slide the deadline.
- Parallel misses of the same ID share a lookup. Other IDs have separate entries.
- GET/HEAD and base/info/image requests share a grant for the same image.
- Permission/path changes may remain invisible until expiry (at most the chosen
  TTL after the lookup starts). This includes replacing a granted source path.
- 403/404, insufficient permissions and invalid paths are returned but not retained.
  Transport/protocol/JSON errors are not cached and remain technical failures.
- After expiry or eviction, API failure never revives an expired grant. A valid
  unexpired grant deliberately continues to work while the API is unavailable.
- A valid permission grant is independent of the login-token lifetime. Login is
  needed only when another permission lookup is necessary.
- Lookup 401 returns a transient outcome carrying the loader's login revision;
  waiters invalidate that generation, then retry once outside the permission
  loader. This preserves the no-nested-load invariant and bounded HTTP budget.
- IDs that would exceed the generic 256-byte cache-key limit are checked without
  permission caching; valid identifiers are not rejected just to fit the cache.

Login and permission entries share the existing **256-entry / 4-MiB** process-memory
budget and LRU eviction. Heavy traffic can evict a grant before its TTL; that causes
another lookup, never a longer authorization window. Restart/new policy snapshot
clears both caches. There is currently **no administrative cache-purge endpoint**;
restart is the available way to clear all grants immediately. Generic Lua
revision-conditional invalidation exists, but needs a deliberately authorized
application entry point before operators can invoke it externally. This does not
revoke already issued media JWTs, whose own expiry remains authoritative.

## Repository mapping and migration differences

The returned source is relative to `iiif-root`:

```text
<path>/<identifier>/derived/<derivativeName>
```

Token fields are `path` and `derivativeName`; API fields use the `shared:` prefix.
Both accept a string or exactly one string in an array. Empty path means the root;
filename is mandatory. Absolute paths, backslashes, control characters, empty
internal components and dot/dot-dot components are rejected, not sanitized.
Identifiers and filenames must each be one safe component. Native local source
resolution additionally checks root containment, including symlinks. This example
returns local files; an S3 deployment needs a deliberate source-table adaptation.

Unlike the Ruby delegate, this policy does not take the first of ambiguous query
or field values, does not repair traversal paths, does not supply a missing
filename as `master.tif`, and does not use anonymous fallback after invalid tokens.
Denials currently use the generic preflight HTTP 403, not a custom 401 challenge.
The base redirect preserves query credentials; info IDs and canonical links do
not. Clients must attach credentials to every subsequent image request. Configure
proxy logging to avoid recording query tokens; the script itself logs no secrets.

See [preflight.md](preflight.md), [lua-jwt.md](lua-jwt.md),
[lua-http.md](lua-http.md), and [lua-configuration.md](lua-configuration.md).

## Container deployment notes

For generic image usage and mounts, see the [Docker guide](docker.md). The
packaged OLDAP environment adapter is optional and must be selected explicitly;
it is not the image's default entrypoint.

The earlier integration trial in the separate oldap-mediaserver project used its
`docker-compose.sipi-test.yml` together with its base Compose file. That override
reused service `imageserver`, its container name and port 8182, allowing the
existing Caddy `/iiif/*` routing to remain in place. SIPI received `/iiif/3/...`
as a retained prefix. In that setup `SIPI_TEST_PUBLIC_ORIGIN` must contain only
the external origin (for example `http://localhost:8088`), without `/iiif/3`, or
service IDs and canonical URLs duplicate the prefix. Other proxy setups must use
the [forwarded-path rules](docker.md#reverse-proxy-and-public-urls) instead of
copying that deployment-specific setting blindly.

Use the sibling project's `docs/sipi-test.md` for its activation and rollback
procedure. The image repository is mounted read-only; the policy does not convert
original files. Container recreation adopts changed environment and policy
snapshots. This historical trial description is not a statement that the current
deployment has been changed by SIPI-2 documentation work.
