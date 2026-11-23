# Metrics

`GET /metrics` returns operational counters in the Prometheus text exposition
format (version 0.0.4, `text/plain; version=0.0.4`, `Cache-Control: no-store`).
It is served directly on the HTTP I/O loop from atomic counters and a brief read
of the queue length, so a scrape never waits for an image worker and still
answers while the worker pool is saturated. No preflight policy runs for it.

Like `/health`, the route is reserved: an IIIF resource named `metrics` cannot be
addressed as a bare base URI. Keep the endpoint private; the
[Caddy example](../examples/operations/Caddyfile) blocks it together with
`/health` on the public mount. Scrape it from the host or an internal network.

## Exported metrics

No label carries request data: there are no identifiers, prefixes, paths,
client addresses or credentials, and every label set is fixed and small.

| Name | Type | Labels | Meaning |
| --- | --- | --- | --- |
| `sipi_build_info` | gauge | `version` | Always 1; the running SIPI-2 version |
| `sipi_iiif_workers` | gauge | | Configured worker threads (`iiif-workers`) |
| `sipi_iiif_active_workers` | gauge | | Workers currently processing a request |
| `sipi_iiif_queue_length` | gauge | | Requests waiting for a worker |
| `sipi_iiif_queue_limit` | gauge | | Configured `iiif-queue-limit` |
| `sipi_iiif_responses_total` | counter | `code` | Requests completed by a worker, by HTTP status: 200, 303, 400, 403, 404, 406, 414, 500, 501, 503 or `other` |
| `sipi_iiif_queue_rejections_total` | counter | | 503 `iiif_busy` because the queue was full (not counted in `responses_total`) |
| `sipi_iiif_disconnected_skips_total` | counter | | Requests skipped because the client had disconnected (also counted as 503 in `responses_total`) |
| `sipi_iiif_processing_seconds` | histogram | `kind` = image, info, base | Worker time from dequeue to response, without queue wait |
| `sipi_preflight_evaluations_total` | counter | `outcome` = allow, deny, error | Lua policy evaluations |
| `sipi_preflight_duration_seconds` | histogram | | Wall-clock time per evaluation, including its `sipi.http` calls |
| `sipi_result_cache_lookups_total` | counter | `outcome` = hit, miss, error | Persistent result-cache lookups (image requests only) |
| `sipi_result_cache_stores_total` | counter | `outcome` = stored, declined, error | Persistent result-cache publications |

Histogram buckets are 1, 5, 10, 25, 50, 100, 250, 500 ms and 1, 2.5, 5, 10 s
plus `+Inf`. Each scrape derives `_count` from the same bucket snapshot as
`+Inf`, keeping them identical during concurrent recording. Requests rejected before reaching the worker (invalid IIIF syntax,
oversized query, wrong method, OPTIONS) are not counted; use proxy logs for
complete HTTP accounting.

## Useful queries

```promql
# Result-cache hit ratio over 5 minutes
sum(rate(sipi_result_cache_lookups_total{outcome="hit"}[5m]))
  / sum(rate(sipi_result_cache_lookups_total[5m]))

# Overload: queue rejections per second and queue occupancy
rate(sipi_iiif_queue_rejections_total[5m])
sipi_iiif_queue_length / sipi_iiif_queue_limit

# 95th percentile of preflight latency (slow authorization backend?)
histogram_quantile(0.95, sum by (le) (rate(sipi_preflight_duration_seconds_bucket[5m])))

# Server errors among completed requests
sum(rate(sipi_iiif_responses_total{code=~"5.."}[5m]))
```

A rising `sipi_result_cache_stores_total{outcome="error"}` together with a
falling hit ratio usually means the cache directory has stopped accepting
writes (see the error log and [persistent cache](persistent-cache.md)).

## Implementation and tests

`src/http/Metrics.*` holds the counters (relaxed atomics, no locks) and renders
the text format; `IiifWorker` records events and supplies the pool state. The
`metrics` CTest checks the exposition format, cumulative histogram buckets,
inclusive bucket bounds and exact totals under concurrent recording.
`worker-queue-http` and `cache-http` scrape the real server and check queue,
disconnect, preflight and cache counters against known request sequences.
