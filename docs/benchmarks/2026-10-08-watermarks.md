# Watermarked tile/cache benchmark — 2026-10-08

## Setup and verification

Apple M5 Max, 18 logical CPUs, 36 GiB RAM; macOS 27.0.1 arm64. Current SIPI
and Conan dependencies built in Release, outside the debugger. Four image workers,
eight persistent HTTP clients, direct loopback requests without Caddy/TLS or OLDAP.
Normal desktop activity remained; no other build or load test ran during measurement.
MinIO ran in the existing local Docker environment. Cloud S3 latency is not modeled.

The existing deterministic 4096x4096 masters share original pixels: a pyramidal
256px-tiled LZW TIFF and lossless JP2 with 1024px tiles/six resolutions. This tests
these fixtures/encodings, not TIFF versus JPEG2000 in general. Each workload has
128 adjacent tile URLs at 1:1 and 1:2, with 256x256 JPEG output. The synthetic
1024x512 gray8 emblem uses white at 25% opacity and 50% image width. It occupies
512 KiB decoded but only 3,319 bytes as PNG; masks with different dimensions or
complexity can have different costs. Not every tile intersects the centered mark. The sources are untagged RGB;
ICC transformation cost, gray/alpha sources and other output formats are not covered.

All responses must match a one-worker, uncached watermarked reference, including
local-versus-S3 equality. A central reference tile must differ from its unmarked
counterpart. Enabled result caches must hold exactly 128 entries after each phase.

Each configuration starts a new process and empty application caches. An unmarked
info request warms image metadata only. Cold = first 128 distinct image requests;
the mask becomes warm during that phase if enabled. OS/MinIO page caches are never
flushed. Warm = repeated URLs in the same process. Without a result cache every
request still renders; without a mask cache every request still decodes the mask.
Every request continues opening/version-checking image and mask, including S3 HEAD.

Three repetitions, deterministic shuffled configuration order. Rates are medians
of per-trial throughput; p95 columns are medians of per-trial p95 latency. RSS is
sampled SIPI resident memory, excluding Docker/MinIO/client and not a true peak.
Throughput includes client hashing/bookkeeping and can become client-limited.

## Rendering without an encoded-result cache

Three repetitions of 512 warm requests per configuration. No image result hits
are possible here; this isolates the mask-cache benefit within actual rendering.

| Source | Backend | Mask off req/s | Mask on req/s | Change | p95 off/on ms |
| --- | --- | ---: | ---: | ---: | ---: |
| master.tif | local | 1922.5 | 2212.5 | +15.1% | 5.34 / 4.64 |
| master.tif | s3 | 196.1 | 232.8 | +18.7% | 58.24 / 43.17 |
| master.jp2 | local | 97.3 | 98.0 | +0.8% | 115.60 / 116.60 |
| master.jp2 | s3 | 62.7 | 63.9 | +1.8% | 162.95 / 157.12 |

The mask cache benefits TIFF rendering here (about 15% locally and 19% over
MinIO). JPEG2000 decode cost dominates: the approximately 1–2% differences are
small relative to run-to-run variation and are not evidence of a material gain.
The result cache remains essential for repeated JPEG2000 tile delivery.

## First requests with empty result caches

These medians come from the all-mode run, 128 unique URLs per trial, including
encoding and synchronous result-cache publication. Both caches enabled:

| Source | Backend | req/s | Range | p95 ms |
| --- | --- | ---: | ---: | ---: |
| master.tif | local | 1098.9 | 1073.3–1157.5 | 8.80 |
| master.tif | s3 | 210.5 | 172.8–214.3 | 46.79 |
| master.jp2 | local | 94.8 | 92.5–95.7 | 117.95 |
| master.jp2 | s3 | 55.4 | 53.1–62.0 | 177.55 |

Cold TIFF fills can be slower than uncached rendering because they include file
publication/fsync and SQLite work. A throughput difference here cannot be assigned
to mask decoding alone. Concurrent initial misses may decode the same mask more
than once; no cold-fill coalescing is implemented for the mask cache.

## Warm encoded-result hits (longer focused run)

The initial 512-request cached phases lasted only about 40–70 ms locally, often
with no RSS sample. They were not used for the following comparison. A separate
focused run repeated **16,384 requests per warm phase**, three times per
configuration. No quantiles from the short phases are pooled into these values.

| Source | Backend | Mask off req/s | Mask on req/s | Change | p95 off/on ms | RSS off/on MiB |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| master.tif | local | 9630.8 | 13385.8 | +39.0% | 1.32 / 1.05 | 23.8 / 21.1 |
| master.tif | s3 | 2075.8 | 3721.6 | +79.3% | 5.54 / 3.23 | 31.5 / 28.0 |
| master.jp2 | local | 9812.4 | 13802.7 | +40.7% | 1.30 / 0.99 | 41.8 / 39.4 |
| master.jp2 | s3 | 2064.2 | 3477.2 | +68.5% | 5.44 / 3.59 | 49.2 / 46.6 |

The mask cache improves warm encoded delivery even though the image no longer
needs decoding: without it, each hit still loads and decodes the required mask.
With it, that work disappears, while current source checks remain. In these runs
mask-on/off throughput ranges do not overlap. Local gains are about 39–41%;
MinIO gains about 68–79%. The similar rates across input formats are expected
when the result cache bypasses image decoding.

S3 cached throughput remains well below local delivery. Current source-version
HEAD requests and the local HTTP/Docker path remain; this is consistent with
additional storage round-trip cost, but the benchmark does not isolate every
component or prove a single bottleneck. Do not extrapolate these MinIO numbers
to AWS latency or use them as a capacity guarantee.

Warm sampled SIPI RSS medians range from about 21 to 49 MiB in the focused run.
The mask cache does not cause a visible memory increase here; allocator retention
and per-request decode allocations affect these snapshots. This is neither a
leak test nor a proof of an RSS bound. The resident-cache budget retains its
documented distinction from active allocations and process RSS.

## Reproduction and retained evidence

See [benchmark instructions](../benchmarks.md#watermarked-tiles-local-storage-and-minio)
for Release dependency preparation, fixture generation and both commands. Runs:

- `build/watermark-benchmark-2026-10-08`: all 16 configurations, three repeats,
  96 phase records, 30,720 measured requests.
- `build/watermark-cache-hits-2026-10-08`: eight result-cache-enabled configurations,
  three repeats, 48 phase records, 396,288 measured requests.

All **427,008 measured responses** matched their serial reference. Status/MIME,
reference dimensions, central watermark presence, cache cardinality and clean
server shutdown were checked. Both runs removed their own temporary MinIO buckets.
References, metadata warmups and setup requests are additional, outside this count.

Raw results/logs remain under ignored `build/`. The initial runner snapshot is
retained with its results; the only later functionality addition was the focused
`--result-cache-only` mode. The checked-in [compact results](2026-10-08-watermarks.json)
retain fingerprints, phase metrics and summaries without per-request records.

Measured executable SHA-256: `625f040edf286d78f794dd37bc75191c51799535b7d5a86224c9236302e49bee`.

## Next step

Keep the current cache defaults and source freshness checks. Before optimizing
S3 reads, measure counts, sizes and time spent in HEAD and Range GET operations
for a cold tile workload. That would distinguish per-request overhead from
transferred-byte cost and provide evidence for read coalescing or buffering.
No production rendering, storage semantics, deployed service or OLDAP policy
was changed by this benchmark step.
