# Resampling kernel and JPEG region decoding — 2026-11-23

## Scope

Before/after comparison for two changes after 1.0.2: the specialized area/linear
resampling kernel (bit-identical output) and JPEG region decoding. The initial
measurements below were made in a
Linux x86-64 container with two vCPUs, GCC 13.3, Release `-O3 -DNDEBUG`, without
native-CPU flags. Dependencies were built from the versions pinned in
`conan.lock` (libjpeg-turbo 3.2.0, libtiff 4.7.2). Shared and noisy hardware:
relative figures are meaningful, absolute ones are not comparable with the
earlier Apple M5 Max records. The corrected-reader section below is a separate
native ARM64 measurement, not a direct comparison with this VM.

## Resampling (`resize-benchmark`)

Synthetic cases of `benchmarks/ResizeBenchmark.cpp`. Two binaries that differ
only in `AreaResize.cpp` ran alternately three times; each value is the median
over the three runs of the per-run medians of nine timed iterations. Output
fingerprints were identical for every case.

| Case | Channels | 1.0.2 ms | New ms | Speed-up |
|---|---:|---:|---:|---:|
| tile-half 512²→256² | 1 | 0.56 | 0.29 | 1.90× |
| tile-fractional 513²→256² | 1 | 1.05 | 0.59 | 1.76× |
| preview 2048×1536→1500×1125 | 1 | 17.98 | 10.58 | 1.70× |
| thumbnail 4096×3072→512×384 | 1 | 12.64 | 9.53 | 1.33× |
| upscale 512×384→1024×768 | 1 | 6.18 | 3.77 | 1.64× |
| tile-half | 3 | 1.21 | 0.64 | 1.89× |
| tile-fractional | 3 | 2.05 | 1.14 | 1.79× |
| preview | 3 | 38.49 | 22.14 | 1.74× |
| thumbnail | 3 | 30.63 | 19.40 | 1.58× |
| upscale | 3 | 16.46 | 9.22 | 1.78× |
| tile-half | 4 | 1.71 | 0.92 | 1.87× |
| tile-fractional | 4 | 2.56 | 1.61 | 1.59× |
| preview | 4 | 49.51 | 30.95 | 1.60× |
| thumbnail | 4 | 35.29 | 27.88 | 1.27× |
| upscale | 4 | 21.97 | 11.79 | 1.86× |

Intermediate steps, single runs: channel specialization alone gave 1.0–1.6×;
replacing the per-channel libm `floor` call in quantization brought most of the
rest. A separable variant (horizontal sums per source row, then a vectorized
vertical pass; exact in the full-image path because all weights are integers)
was slower in that configuration (preview RGB 20.6 → 24.1 ms, thumbnail RGB
18.3 → 25.4 ms) and was not kept.

## JPEG region decoding

**Historical, superseded measurements:** these timings used a decoder that
skipped directly to the bottom, bypassing remaining entropy data and EOI checks.
The corrected decoder validates the complete stream. Do not use these figures
as performance claims for the corrected implementation; see the new measurement below.

Standalone timing of `read_jpeg_image` versus `read_jpeg_region` on generated
6000×4000 RGB images (quality 90, 4:2:0), median of seven runs.

| Request | Baseline JPEG ms | Progressive JPEG ms |
|---|---:|---:|
| Full decode | 188.5 | 450.1 |
| 512×512 tile, centre | 57.7 (3.3×) | 376.4 (1.2×) |
| 1024×1024 crop, centre | 70.0 (2.7×) | 366.8 (1.2×) |
| 512×512 tile, top-left | 15.5 (12.2×) | 365.3 (1.2×) |
| Full-width band of 500 rows | 61.6 (3.1×) | 370.0 (1.2×) |

In this measured implementation, rows above the region passed through Huffman
decoding while skipping to the bottom bypassed remaining entropy data. Progressive images read every scan into
coefficient buffers before the first row is available, which bounds the gain.

## Corrected JPEG reader: complete tail validation, native ARM64

Measured again on 2026-11-23, Apple M5 Max, macOS ARM64, Apple Clang 21.0.0
(`clang-2100.4.1.1`), `-O3 -DNDEBUG`, pinned libjpeg-turbo 3.2.0 Release static
library from Conan. One benchmark process, one decode at a time; no simultaneous
build/test workload was launched during the timing runs.

The reproducible [benchmark](../../benchmarks/JpegRegionBenchmark.cpp) generates
6000×4000 RGB images, quality 90, 4:2:0, baseline and progressive, from the same
deterministic gradient/noise pattern. The original VM generator was unavailable:
these are new fixtures, so neither absolute times nor speed-up changes isolate
the tail fix from hardware and image-content differences.

The reference is the **current full decoder** (`read_jpeg_image`), not an old
1.0.2 binary. Both paths validate the complete stream. Each case has two warmup
pairs followed by seven measured pairs; full/region order alternates. Three
independent invocations give 21 measured pairs per case/coding. The table reports
medians of those 21 samples; speed-up is full median / region median. Each case
has its own paired full-decode measurements. Metadata/source opening, fixture
encoding, pixel comparisons and output destruction are outside the timed region;
decode allocations and destruction of internal decoder state are included.
The OS file cache is warm. This is decode-only, not HTTP or end-to-end latency.

Every measured output and warmup was compared sample-for-sample with a full
decode: all matched. The reader skips at most to the penultimate row, reads the
last row and finishes through EOI. Truncated-tail regressions remain covered by
`jpeg-region`; timing does not replace those correctness checks.

| Coding | Request | Full decode ms | Region decode ms | Speed-up |
|---|---|---:|---:|---:|
| Baseline | Full (control) | 92.20 | 92.94 | 0.99× |
| Baseline | 512×512 centre | 90.02 | 72.60 | 1.24× |
| Baseline | 1024×1024 centre | 91.12 | 73.93 | 1.23× |
| Baseline | 512×512 top-left | 91.68 | 73.74 | 1.24× |
| Baseline | 6000×500 central band | 91.85 | 75.71 | 1.21× |
| Progressive | Full (control) | 245.98 | 244.53 | 1.01× |
| Progressive | 512×512 centre | 248.90 | 232.77 | 1.07× |
| Progressive | 1024×1024 centre | 247.46 | 235.58 | 1.05× |
| Progressive | 512×512 top-left | 243.30 | 228.40 | 1.07× |
| Progressive | 6000×500 central band | 248.50 | 234.05 | 1.06× |

The measured gain is modest: 1.21–1.24× for baseline crops (about 18–20% less
decode time), 1.05–1.07× for progressive crops (about 5–6% less). The full-image
control is 0.99–1.01×, consistent with the same underlying path and timing noise.
Results depend on image entropy, codec SIMD and hardware; they do not establish
production latency or cold-storage throughput.

The output raster still shrinks from 72,000,000 bytes for the full RGB image to
786,432 bytes for a 512×512 tile, plus one scratch row (at most 18,000 bytes).
Progressive coefficient buffers and other codec allocations remain: this is not
an RSS measurement or a claim of proportional process-memory reduction.

[Raw samples](2026-11-23-jpeg-tail-samples.csv) retain all 210 measured pairs.
Local fixtures, source/fixture SHA-256 hashes and compiler metadata are retained
under `build/jpeg-tail-benchmark/`. No private images are required.

To reproduce after configuring the Release build as in [building](../building.md):

```sh
cmake --build build/Release --target jpeg-region-benchmark --parallel 4
mkdir -p build/jpeg-tail-benchmark
for run in 1 2 3; do
  build/Release/jpeg-region-benchmark build/jpeg-tail-benchmark/images 7 \
    > "build/jpeg-tail-benchmark/run-${run}.csv"
done
```

The recorded run compiled the same harness, `JpegReader.cpp` and
`LocalFileSource.cpp` directly with the flags above and linked the Conan Release
`libjpeg.a`. The CMake target is excluded from ordinary builds and was separately
build-verified; do not use a Debug or sanitizer binary for performance numbers.

## Effect on delivered bytes

A differential HTTP comparison (3,162 request pairs: all fixtures, regions, sizes,
rotations, qualities, formats, watermarks, restricted variants, error cases; cache
miss and hit) against the 1.0.2 binary found byte-identical responses except 48
plain JPEG-source crops. Decoding them pixel by pixel showed 323 of 1.36 million
samples differing by exactly 1 (region-relative resampling coordinates); see
[JPEG input](../jpeg-input.md#region-decoding).
