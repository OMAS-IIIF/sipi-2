# Operating SIPI-2

This guide covers running an already built or published SIPI-2 service. Use the
[native configuration guide](command-line.md) or [Docker guide](docker.md) for
installation paths, mounts and startup commands. All settings, scripts and secret
files are startup snapshots; restart SIPI after changes.

## Deployment boundary

Run SIPI behind a reverse proxy that terminates TLS. Bind a native listener to
loopback when the proxy is on the same host; the generic Compose example publishes
only a loopback port. If both services are containers, use a private shared network
and the SIPI service name/port. A proxy container's `127.0.0.1` refers to itself.
Do not expose an additional backend port that bypasses the intended public proxy.

Choose a dedicated runtime account and working directory, read-only image/policy
mounts where practical, and a separate writable local cache. Native execution has
no fixed UID requirement; the published container uses UID/GID 10001. Provision
permissions explicitly. Drogon may create `uploads/` in the working directory;
this does not enable uploading. Use a service manager or Compose restart policy to
supervise the process and retain stdout/stderr. SIPI provides no daemonization,
pid-file, hot-reload or built-in log-rotation workflow.

## Reverse proxy, TLS and public identity

The [Caddyfile example](../examples/operations/Caddyfile) exposes `/iiif/*`, removes
that mount before forwarding and leaves health/demo routes private. An explicit
[`route` block](https://caddyserver.com/docs/caddyfile/directives/route) preserves
the order: reject private paths before stripping the mount and proxying. For a proxy
running on the host, set `SIPI_SITE` to your DNS name and `SIPI_UPSTREAM` to the
loopback listener (default `127.0.0.1:18080`). These two variables configure Caddy,
not SIPI. For example, configure SIPI with:

```toml
listen-address = "127.0.0.1"
port = 18080
public-base-url = "https://images.example.org/iiif"
```

Replace the example hostname consistently in both configurations. Keep repository,
Lua and cache settings from your deployment. A container's listener must instead
accept traffic on its internal interface; do not override the image's
`0.0.0.0` bind with loopback for a separate proxy container.

Caddy's [`handle_path`](https://caddyserver.com/docs/caddyfile/directives/handle_path)
strips the matched mount prefix. Therefore a public request for
`/iiif/project/sample.png/info.json` reaches SIPI as
`/project/sample.png/info.json`; Lua sees prefix `{ "project" }`, not `{ "iiif", "project" }`.
The resulting public `id` includes `/iiif` exactly once. If your proxy instead
retains that segment, use an origin-only public base URL and account for `iiif` in
Lua's prefix policy. The [Docker URL table](docker.md#reverse-proxy-and-public-urls)
compares both cases. Host and Forwarded headers cannot override SIPI's configured
public identity.

With a real public hostname, Caddy can obtain and renew certificates and redirect
HTTP to HTTPS. Its standard setup needs correct DNS, reachable ports 80/443 and
persistent writable certificate storage; follow
[Caddy's automatic HTTPS requirements](https://caddyserver.com/docs/automatic-https).
Keep that storage across proxy recreation. Validate the adapted configuration
before starting/reloading an existing proxy, using its installed Caddy version:

```sh
caddy validate --config examples/operations/Caddyfile --adapter caddyfile
```

This checks configuration, not DNS reachability, certificate issuance or image
access. Adapt an existing site's routing instead of replacing unrelated sites.
The example's `/iiif` and `/iiif/` are not collection indexes; they do not list images.

Preserve Authorization, Cookie, Origin, Accept and the query when proxying; do not
add a second decoding pass or rewrite encoded identifier separators. Caddy's
[reverse proxy documentation](https://caddyserver.com/docs/caddyfile/directives/reverse_proxy)
explains forwarding behavior. Check encoded identifiers and repeated query values
with the actual proxy. Keep credential-bearing queries out of access logs. The
example does not enable access logging; configure retention/redaction if adding it.

Use SIPI's [CORS settings](iiif-http.md#configurable-cors) consistently. A browser
cookie flow requires explicit allowed origins, credentials mode and suitable
application cookies; CORS does not authorize images. Avoid conflicting proxy-added
CORS headers. Preserve `Cache-Control: no-store`, `Vary`, `Link` and `Location`.
Do not force shared proxy/CDN caching of protected responses: it would bypass the
next request's Lua check. SIPI's internal result cache already reuses image work
after authorization.

## Monitoring: liveness and useful delivery

| Probe | What it establishes | What it does not establish |
| --- | --- | --- |
| Private `/health` | HTTP loop responds with status/version | Image-worker availability, repository, Lua backend or decoding |
| Authorized `info.json` | Policy, source access and metadata admission | Pixel integrity of the whole source |
| Small representative image GET | Policy, source and delivery for that request | All formats/regions or cold decoding after a cache hit |
| Expected-denied request | That policy still refuses this identity/credential case | Every authorization rule |

Run host-side probes or an external monitor; the published image contains no curl
and defines no Docker HEALTHCHECK. A HEAD image request can perform rendering and
fill the cache before suppressing the body, so it is not a cheap readiness shortcut.
A healthy HTTP loop can coexist with a full image-worker queue.

For the sample-only policy from `examples/docker/`, on a host proxy with the
configuration above (adapt identifiers and authentication to your own policy):

```sh
curl --fail --silent --show-error --max-time 5 http://127.0.0.1:18080/health
curl --fail --silent --show-error --max-time 15 \
  https://images.example.org/iiif/sample.png/info.json
curl --fail --silent --show-error --max-time 30 --output /dev/null \
  https://images.example.org/iiif/sample.png/full/160,/0/default.jpg
curl --silent --show-error --max-time 15 --output /dev/null --write-out '%{http_code}\n' \
  https://images.example.org/iiif/not-allowed/info.json
```

Expect health `status=ok` and the intended version, an external HTTPS `id` in the
information document, successful image delivery and 403 for the denied sample.
The alias policy from `examples/preflight.lua` instead uses identifier `example`.
Include valid monitoring credentials for protected policies and test a refused
case separately. Do not disable authorization to make a probe pass.

Track external request latency and status rates together with process restarts,
CPU/RSS, free disk space, cache-directory growth and backend errors. SIPI has no
built-in Prometheus/metrics endpoint or separate readiness endpoint. Avoid
high-cardinality image IDs or credentials in metric labels. A warm thumbnail probe
is useful for availability; a controlled cold render is needed to exercise decoding.

## Logs and failure diagnosis

Use `log-level=info` for normal operation and short, deliberate debug sessions for
cache/render diagnosis. Capture both stdout and stderr with the service manager;
rotate and limit retention there. With the Compose project from the Docker guide:

```sh
docker compose -p sipi-demo -f docker/compose.example.yaml logs --tail=200 sipi
```

Use the same environment and project name that created your deployment. Debug logs
include cache hit/miss/stored/declined messages, but no public cache-status header
or metrics counters are provided. A declined cache admission is not a delivery
failure. Logs can contain script path/line and masked Lua diagnostics; see
[diagnostic limits](lua-configuration.md#limits-and-diagnostics). Never log whole
request headers, tokens, secret values or permission-service bodies from Lua.

| Symptom | First checks |
| --- | --- |
| Startup exits 2 | Selected CLI/environment/TOML, existing repository, readable script/secret files and Lua initialization diagnostic |
| Proxy 502/504 | Backend process/listen address, host versus container network, port and upstream timeout; then direct private probe |
| Healthy `/health`, image 503 `iiif_busy` | Worker saturation/64 waiting requests, slow storage or Lua HTTP backend; reduce incoming load before raising workers |
| 403 `access_denied` | Lua's prefix/identifier mapping and credentials, including token expiry/clock; info/base/HEAD also run policy |
| 403 `source_not_allowed` | Relative path containment and symlinks escaping the root |
| 404 `image_not_found` | Resolved filename/key, case, source existence and regular-file status; prefix is not automatically part of storage mapping |
| 500 `source_not_configured` | Local root or S3 bucket/backend configuration for the source chosen by Lua |
| 500 `preflight_failed` | Lua error category, return contract, required settings or resource budgets; inspect masked server diagnostic |
| 500 `watermark_failed` | Required mask exists, is readable and has a supported format; no unmarked fallback |
| 500 `invalid_image_source` or `source_error` | Image validity, read permissions, stable source writes, storage/backend errors |
| 400 or 501 image response | Error code plus supported IIIF operation, output limits, input layout and output format in the feature catalog |
| Wrong origin, missing/duplicated `/iiif` | Explicit public-base-url and whether the proxy strips its mount |
| Browser fails while curl works | CORS Origin/credential settings, viewer credential forwarding, cookie policy and HTTPS |
| Cache warning but 200 image | Storage/lock/schema/capacity issue; delivery may continue uncached |

Separate proxy-generated responses from SIPI JSON errors. Do not convert all
backend failures into anonymous access or retry denied requests indefinitely.
See the [feature catalog](features.md), [HTTP contract](iiif-http.md) and
[Lua guide](preflight.md) for supported behavior.

## Capacity, timeouts and shutdown

Set `iiif-workers` explicitly for the resources allocated to the service. The
default is logical CPUs clamped to 1–4; accepted values are 1–32. The queue holds
64 waiting jobs globally; overload returns 503 `iiif_busy` with `Retry-After: 1`.
More workers multiply active decoder/raster/Lua memory and storage traffic. Output
limits and cache byte budgets do not bound total process RSS. Measure your real
image sizes/formats and cold/warm mix before increasing concurrency.

Align proxy/client timeouts with permitted image work and Lua HTTP limits. Lua's
instruction budget does not bound native decoding or storage wall-clock time;
outbound Lua HTTP has its own [timeouts and aggregate budgets](lua-http.md).
A client timeout/disconnect is not a promise of immediate work cancellation.
Avoid aggressive retries that increase saturation.

For planned maintenance, remove the instance from proxy/load-balancer rotation
and allow in-flight traffic time to finish, then stop it with the service manager
or Docker. A normal native foreground run stops with Ctrl-C. On HTTP-loop exit,
SIPI rejects new jobs, discards waiting jobs and joins active workers; it does not
guarantee that queued requests receive a final response during shutdown. Active
native work is not forcibly cancelled. Choose the supervisor's stop grace period
from observed work duration, rather than assuming a fixed instant drain.
See [worker lifecycle](worker-pool.md#lifecycle-and-verification).

## Cache maintenance and recovery

The [persistent result cache](persistent-cache.md) is regenerable local storage,
not a backup of original images. Each running instance needs its own cache directory
or volume. A lock prevents simultaneous ownership; network/shared cache storage is
unsupported. Do not delete `cache.lock`, edit SQLite or remove object files while
its owner is running. There is no administrative purge API.

Normal LRU eviction needs no cron cleanup. `cache-max-bytes` counts encoded bodies;
SQLite, journals and filesystem overhead are additional. Publication checks free
space with 64 MiB headroom, which is not a disk quota. Monitor physical disk usage
and leave room for the operating system and other writers. Reducing the configured
budget takes effect on restart. Either zero limit disables an individual memory
cache's retention; to disable persistent caching, omit `cache-dir` from all active
configuration sources.

If the cache cannot initialize, SIPI warns and can continue uncached; `/health`
still succeeds. Startup recovery reconciles incomplete publications, but does not
automatically rebuild a corrupt/foreign/unsupported SQLite index. For recovery:

1. Confirm the exact cache path/volume, distinct from original media and secrets.
2. Drain and stop its owning instance. Preserve the old cache if diagnosis is needed.
3. Select a new empty dedicated directory/volume with the correct permissions and
   update the effective configuration. Keep the old directory intact during verification.
4. Restart, check initialization logs and run the image/deny probes. Expect a cold cache.
5. Remove the retired cache only after confirming it is unused and regeneration works.

Back up originals, masks, policies, configuration, secret-management references and
release identity. Encoded caches normally need no backup. If retaining a cache
snapshot, take it offline as one whole directory; copying only the SQLite file is
not a usable image-store backup. RAM metadata/mask/variant caches and `sipi.cache`
are lost on restart; do not use the Lua TTL cache as an authoritative permission store.

Publish source updates atomically rather than overwriting images being read.
Source version checks run before result hits; missing or changed originals cannot
be replaced by serving stale cache entries. Repository directories/symlinks must
remain operator-controlled; see [source ownership](source-resolution.md#filesystem-boundary).

## Updates, rollback and acceptance

Record the running binary version or pinned container digest together with policy,
configuration and dependency assumptions. Avoid mutable `latest` for a reproducible
rollout. Test a candidate separately with its own cache; verify allowed and denied
requests, external IDs/redirects, CORS and representative real image formats.
Do not run both instances against one cache directory.

Drain the old instance, apply the selected binary/image and matching configuration,
restart and repeat probes through the public proxy. Changing a bind-mounted single
file atomically may require container recreation to see the replacement; a process
restart alone does not replace that bind mount. For rollback, retain the previous
binary/image plus compatible configuration/policy. If cache compatibility is
uncertain, select a fresh cache rather than modifying its schema. See the
[Docker update procedure](docker.md#logs-updates-and-troubleshooting).

Before opening traffic, check public DNS/TLS and certificate renewal storage,
correct info/redirect/canonical URLs, browser CORS and credentials, expected denials,
representative images, backend reachability, cache ownership and observability.
A local proxy test does not validate your public DNS, certificate issuance or
production service-manager setup.

## Verification scope

The example was configuration-validated with Caddy 2.8.4 and exercised against
published SIPI-2 0.1.3 on native ARM64 in isolated containers. Twenty-four local
HTTP checks passed: mount stripping, external info/redirect identity, encoded
prefixes, forwarding-header isolation, GET/HEAD, CORS/OPTIONS, blocked health/demo
routes and JWT authorization after cache fill. Test containers and networks were
removed. This verifies the example with an existing local Caddy version; it does
not recommend that version as a current production release. Public DNS/ACME,
certificate renewal, browser-cookie behavior and production stop-grace sizing
remain deployment acceptance checks.

Focused existing native tests also passed: `result-cache`, `cache-startup`,
`iiif-http-protocol` and `iiif-worker` (plus two fixture setup tests). These cover
cache lifecycle/fallback and HTTP/worker behavior; no fresh compilation or external
S3/authorization service test was performed for this documentation step.
