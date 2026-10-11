# Home x86 VM archive render-core comparison — 2026-10-12

## Validation and scope

The user ran the prepared twelve-file corpus with the same AMD64 image on
`media.home.org`. [Unmodified results](2026-10-12-x86-archive-resize.json) retain
all samples, fingerprints and environment details. Binary hashes and source
manifest exactly match the preceding synthetic run. All twelve file SHA-256 values
match the ARM archive comparison. Four ABBA runs each contain 51 cases, two warmups
and nine measured samples per case: 1,836 matching pixel outputs, including 1,296
matching JPEG outputs. Case sets, counts, positive timings, medians and comparisons
were rechecked on import.

The VM exposes four generic QEMU vCPUs, without AVX/AVX2; the physical host CPU
cannot be inferred from its virtual CPU vendor string. GCC 13.3.0 Release, one
benchmark/Grok worker, no container CPU quota, 2 GiB memory limit. Same scope as
the [ARM archive benchmark](2026-10-12-axis-weights.md): warm local sources and
reused metadata/handles, decode + resize + JPEG 90/4:4:4. This excludes HTTP,
authorization, source opening, metadata planning, result caches and S3. It is not
an end-to-end service latency or throughput benchmark.

## Results

Arithmetic means over six images × nine samples × two runs per version, matching
the ARM report's aggregation. The raw runner's per-image comparisons instead use
the mean of each run's median; do not mix these aggregation methods.

| Source | Request | Resize before ms | Resize after ms | Resize speedup | Core before ms | Core after ms | Core time reduction |
|---|---|---:|---:|---:|---:|---:|---:|
| TIFF | thumbnail | 0.977 | 0.829 | 1.18× | 2.224 | 2.177 | 2.1% |
| TIFF | preview | 30.575 | 24.219 | 1.26× | 59.960 | 54.399 | 9.3% |
| TIFF | tile | 1.655 | 1.282 | 1.29× | 5.361 | 5.190 | 3.2% |
| JP2 | thumbnail | 1.085 | 0.890 | 1.22× | 52.204 | 51.406 | 1.5% |
| JP2 | preview | 30.713 | 24.473 | 1.26× | 626.927 | 618.914 | 1.3% |
| JP2 | tile | 1.641 | 1.286 | 1.28× | 85.181 | 84.760 | 0.5% |

TIFF preview rendering shows the clearest overall improvement: the two before-run
core means are 59.883/60.036 ms and after means 54.389/54.409 ms. Scaling saves about
6.36 ms, partly offset by about 0.90 ms more decoding time. TIFF tile scaling also
improves consistently, but the total gain is much smaller.

TIFF thumbnails are mixed. Their core run means are 2.225/2.224 ms before versus
2.111/2.243 ms after. In particular, `sample-tiff-02` has a per-image median-based
core increase from 1.564 to 1.879 ms; decode and encode also slow down, although
neither implementation changed. Its resize median increases from 0.740 to 0.772 ms.
Thus the aggregate 2.1% gain is not a reliable general thumbnail improvement, and
not every input benefits. VM/load/cache variation and binary-layout effects are
possible explanations, not established causes.

JP2 preview scaling saves about 6.24 ms, but decoding alone still costs about
588 ms. Its approximately 1.3% core improvement includes fluctuations outside
resizing; the smaller thumbnail/tile differences should not be overinterpreted.
No absolute CPU-architecture ranking or general TIFF-versus-JP2 claim follows:
these files, compression settings, compiler and VM define this experiment.

## Decision

The modest, portable axis-weight optimization is justified on both tested
platforms, with identical measured outputs. Stop performance work here for now;
retain the current simple implementation and defer explicit SIMD. Further work
should follow a concrete production bottleneck, not a microbenchmark target.
This step only records measurements; no code, Docker image or deployment changed.
