# OLDAP / OpenSeadragon measurement — 2026-10-07

## Finding

With the rendered-image cache warmed, the existing media-token path delivers the
same images much faster than anonymous access. Sequential request medians were
**1.73 ms with a media token**, **24.34 ms anonymously**, and **20.53 ms for the
standalone OLDAP permission lookup**. This points to external authorization as the
main remaining cost of anonymous delivery in this local setup. It does not justify
silently caching permissions or removing authorization checks.

## Actual browser workload

Two independent viewer captures used the installed OpenSeadragon **6.1.1** from
FasnachtsPage, a 960 × 600 CSS-pixel viewport, device pixel ratio 2, and the Codex
in-app Chromium browser (reported Chrome 154). The measurement page uses the
application's navigator, animation, blending and viewport constraints; it is a
standalone measurement page, not the complete Svelte application.

Image: `UtCbbUM5Po4g`, 2400 × 1200, existing `derived/master.tif`. The actual viewer
opened `info.json`, showed the overview, zoomed 2×, panned left, zoomed another 2×,
panned right, and returned home. Both captures produced the same **nine distinct
JPEG URLs plus info.json**, with **no tile errors**. OpenSeadragon selected its
own regions and sizes from the current info response; no synthetic tile grid or
`tiles` declaration was substituted.

Initial browser JPEG resource durations ranged from 208–351 ms; the repeat ranged
from 138–295 ms. These are real browser resource timings, including browser
scheduling/transport and potentially cache behavior. They are not server-stage
measurements. The roughly 6.4-second scripted interaction includes deliberate
one-second settling intervals, so it must not be reported as image load time.
The first capture is **not** a controlled cold-cache benchmark: existing server,
filesystem and browser caches were neither removed nor flushed.

## Controlled replay results

Each capture's nine unique JPEG URLs was warmed once before replay. Five rounds
per capture used one or six concurrent Python clients, alternating mode order:

- **Anonymous:** Caddy → SIPI Lua policy → cached login token → real per-image
  OLDAP lookup → image delivery.
- **Media token:** Caddy → SIPI Lua JWT validation → image delivery. The runner
  locally signs a short-lived image-scoped test token using only the path and
  permission returned by the successful anonymous API lookup. The key and token
  remain in memory; the token travels in Authorization, never in a URL or report.
- **API only:** direct host HTTP requests to the same OLDAP media-object endpoint,
  with an anonymous login token obtained before timing. No Caddy/SIPI involved.

Pooled per-request median and nearest-rank p95 across both captures: **90 samples
per row**. Batch time is the median of ten rounds, each containing nine requests.
Client timing includes opening a fresh TCP connection and reading the complete
body; executor waiting is excluded from per-request latency but included in batch
time. Both image modes use identical connection and SHA-256 verification logic.

| Clients | Mode | Request median ms | Request p95 ms | Nine-request batch median ms |
| ---: | --- | ---: | ---: | ---: |
| 1 | Anonymous | 24.34 | 29.41 | 223.63 |
| 1 | Media token | 1.73 | 4.17 | 17.38 |
| 1 | API only | 20.53 | 24.42 | 187.05 |
| 6 | Anonymous | 69.72 | 134.64 | 149.05 |
| 6 | Media token | 4.73 | 11.24 | 8.62 |
| 6 | API only | 77.25 | 103.44 | 130.97 |

All **360 measured image responses** returned 200 and matched their URL's warmed
reference SHA-256 across both authorization modes. All **180 timed standalone API
lookups** returned 200. Sequential medians remained stable between the two runs:
anonymous 24.68/23.89 ms, media token 1.71/1.75 ms, API 20.87/20.24 ms.

Two separate, untimed-for-replay anonymous login probes took 17.45 and 11.69 ms.
These are individual observations, not a login latency distribution or a measured
before/after comparison of the new login cache. Exact live SIPI login counts were
not instrumented here; concurrent single-login behavior is covered by the mock-API
tests introduced with the cache.

## Setup and interpretation limits

- Existing local Docker trial, Linux ARM64 Release, GCC 13; Docker reports 18 CPUs,
  container memory limit 2 GiB. Default SIPI worker count is four. Caddy listens on
  localhost:8088; SIPI uses internal port 8182; OLDAP API is on host port 8000.
- Image ID: `sha256:1d8c87df183d9d482631968498e2673e17b4ae02f8594fe7f77cd8f2f1ccd73a`.
- No deployment, API, frontend, credentials, worker setting or cache configuration
  was changed. Normal requests naturally add rendered results and update access
  times. The source is mounted read-only. No concurrent builds ran during timing.
- Replay deliberately bypasses browser caching but uses fresh client connections,
  unlike browser keep-alive. Its numbers must not be presented as browser latency.
- The API-only path uses host loopback; SIPI reaches it from Docker. Six direct API
  clients also differ from SIPI's four-worker admission. Consequently the medians
  are supporting comparisons, not exactly subtractable timing components.
- API concurrency improves batch time modestly and increases individual latency.
  This does not identify whether API, database, connection handling or another
  internal component serializes work; profiling is needed before claiming why.
- No per-stage server instrumentation, controlled empty-cache run, codec comparison,
  production-network test, S3 run or Cantaloupe comparison was performed. Identical
  warmed URLs are expected to hit the configured cache, but individual cache-hit
  counters were not captured. Do not infer raw rendering speed from these numbers.

A useful next decision is whether anonymous viewers should receive an explicitly
short-lived, image-bound media token after the initial permission check. SIPI
already supports that fast path. Such a change needs agreed expiry/revocation
semantics and frontend/API integration; this measurement does not implement it.
Alternatively, profile the existing OLDAP lookup before changing its behavior.

## Reproduction and artifacts

Run from the SIPI repository with the existing local OLDAP Docker trial active:

```sh
python3 benchmarks/oldap_viewer.py \
  --osd ../OLDAP/FasnachtsPage/node_modules/openseadragon/build/openseadragon/openseadragon.min.js \
  --output build/oldap-viewer-new-run
```

The output directory must be new. Open the printed loopback URL in a browser and
click **Start measurement**. Browser capture starts the replay automatically; the
runner prints summaries and closes its temporary listener when finished. It uses
only Python's standard library, Docker inspection and the already installed OSD
bundle. No package installation or signing-key copy to disk is needed. The fixture
assumes the current local OLDAP environment contract and passwordless unknown user.

Raw resource traces, samples, status and image identity remain in ignored files:

- `build/oldap-viewer-2026-10-07/results.json`
- `build/oldap-viewer-2026-10-07-repeat/results.json`

The runner and HTML measurement page are retained under `benchmarks/`; the table
above preserves the compact results independently of ignored local artifacts.

## Follow-up: anonymous permission cache enabled

After implementing the Lua permission cache (default 600 seconds), an additional
local replay used the same nine recorded JPEG URLs, warmed first, with five
rounds each at one and six clients. All 90 responses matched their reference bytes;
an invalid supplied token remained 403. Sequential anonymous median was **1.38 ms**
(p95 2.55 ms), and the six-client batch median was **5.99 ms** (individual median
3.20 ms, p95 11.14 ms). Raw samples: `build/oldap-permission-cache-check.json`.

This shorter follow-up supports the expected removal of the repeated REST cost;
it is not a controlled simultaneous A/B experiment. It records a different image
build and five rather than ten rounds. No new browser capture was required because
the replay used the existing actual viewer trace. The runner now measures whatever
permission TTL the deployment configures. Reproducing the original uncached-policy
baseline requires explicitly setting `anonymous_permission_ttl=0` and restarting
that test deployment; do not compare modes without recording that policy setting.
