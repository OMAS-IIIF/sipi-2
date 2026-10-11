# Cached axis weights — 2026-10-12

## Change

The resampler now prepares a compact support for each horizontal output position:
first/last contributing source pixel, two edge weights and a common interior
weight. Vertical support is calculated once per output row. Full-image reduction
retains integer overlap weights; fractional reduction retains the previous exact
rational boundaries and edge-overlap formulas. Enlargement retains pixel-center
mapping and clamping. The same support represents all three paths.

No intermediate raster, second resampling pass, channel specialization, explicit
SIMD or fast-math. The pixel contribution order, weight multiplication order,
alpha weighting and quantization remain unchanged. This is not a filter change.

The optional horizontal vector costs `width * sizeof(AxisWeights)` (40 bytes per
output column on this build). It is allocated only within the remaining source +
output + ICC working-memory budget. If it does not fit, supports are computed on
demand using the same formulas. Vertical state is constant-sized; identity remains
allocation-free. No new configuration and no newly rejected tight-budget request.

## Measurement

Same [baseline harness and scope](2026-10-11-resize.md), native Apple M5 Max ARM64,
Release `-O3 -DNDEBUG`, JPEG 90/4:4:4, six prepared archive TIFF/JP2 pairs.
Four sequential runs: before → after → after → before; the last pair reverses
file order. Two warmups and nine timed iterations per case per run. No builds or
other deliberate load tests ran concurrently with timing. Desktop/frequency/cache
variability is not eliminated. These are warm local render-core timings, excluding
HTTP, authorization, result-cache lookup and source opening/metadata planning.

All 1,836 measured pixel outputs have stable matching fingerprints across versions;
all 1,296 measured JPEG outputs match too. Before/after raw samples, file hashes,
binary/source hashes and environment are in [the JSON report](2026-10-12-axis-weights.json).
The earlier baseline alone is not used as the timing comparator; both binaries
were rerun in this session. No claim is made for Intel/AMD until measured there.

Arithmetic means over six images × nine samples × two runs per version:

| Source | Request | Resize before ms | Resize after ms | Resize speedup | Core before ms | Core after ms |
|---|---|---:|---:|---:|---:|---:|
| TIFF | thumbnail | 0.457 | 0.340 | 1.35× | 1.529 | 1.343 |
| TIFF | preview | 12.208 | 10.210 | 1.20× | 36.713 | 34.631 |
| TIFF | tile | 0.532 | 0.500 | 1.07× | 3.681 | 3.671 |
| JP2 | thumbnail | 0.446 | 0.358 | 1.25× | 37.615 | 37.485 |
| JP2 | preview | 12.136 | 10.258 | 1.18× | 449.486 | 446.363 |
| JP2 | tile | 0.529 | 0.485 | 1.09× | 63.342 | 62.283 |

The TIFF core is about 12% shorter for thumbnails and 6% for previews in this
sample. Tiles are essentially unchanged as whole requests. JP2 remains decoder
bound; its small total differences should not be attributed confidently to this
optimization. Kernel speedup is not the same as server throughput speedup.

Isolated kernel speedup: ratio of the mean of two per-run medians before/after.

| Case | Gray | RGB | RGBA |
|---|---:|---:|---:|
| tile-half | 1.11× | 1.14× | 1.15× |
| tile-fractional | 1.36× | 1.05× | 1.05× |
| preview-noninteger | 1.01× | 1.19× | 1.13× |
| thumbnail-eighth | 1.05× | 1.17× | 1.17× |
| upscale-double | 2.33× | 2.36× | 2.24× |

Gray noninteger preview reduction is effectively unchanged; not every workload
benefits. Enlargement benefits most because expensive repeated coordinate
preparation previously outweighed its four-pixel interpolation.

## Correctness and memory verification

- Existing independent area oracle, fractional-window, interpolation, alpha and
  memory-limit tests retained.
- New deterministic 4,000-case test across gray, gray-alpha, RGB and RGBA compares
  cached supports against exact-budget on-demand supports, including large rational
  denominators, reduction, enlargement and mixed axes. One-byte-under-budget
  requests still fail (identity is handled separately).
- The same 4,000 cases were also compared byte-for-byte with a separately compiled
  snapshot of the old implementation, with public function names renamed to avoid
  symbol collisions. Temporary source/driver/binaries are retained under `build/`;
  production code does not carry a duplicate reference implementation.
- Full Debug and Release CTest suites each pass 71/71 tests. The 4,000-case
  cached/on-demand comparison also passes AddressSanitizer and
  UndefinedBehaviorSanitizer. Results are recorded in CODEX_LOG.md.

## Reproduce and scope

Build `resize-benchmark` as in the baseline document, preserve the old executable
before rebuilding the new version, and run both on the same files in ABBA order.
The report records each file list and nine samples per case. Run correctness with
`ctest --test-dir build/release -R 'area-resize|resize-weights|upscale' --output-on-failure`.

The change is confined to resampling. Codec selection, IIIF policy, output quality,
cache identity and deployment settings are unchanged. No deployment performed.
The next optional step is native x86 measurement of this same harness; explicit
SIMD and rotation optimization are deferred rather than presumed necessary.
