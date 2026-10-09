# Real-image TIFF and JPEG2000 comparison

`benchmarks/archive_compare.py` extends the [synthetic comparison](server-comparison.md)
with the six independently verified [archive image pairs](archive-test-corpus.md).
It reuses container ownership, explicit resource limits and HTTP timing from
`server_compare.py`. No delivery code or existing OLDAP service is changed.

Recorded run: [2026-10-08 results](benchmarks/2026-10-08-archive-servers.md), with
192 complete trials and 1,201,230 verified measured responses.

## Reproduce

Prepare `build/archive-images` first. The runner verifies the manifest and the
SHA-256 of every prepared TIFF/JP2 before and after the run. It creates hardlinks
in its new output directory, mounted read-only in containers. Keep the corpus
unchanged on the host while running; hardlinks are not independent copies.

Cantaloupe needs a working JPEG2000 decoder. Build this local benchmark derivative
of the existing Cantaloupe 5.0.7 image:

```sh
docker build --platform linux/arm64 \
  -f benchmarks/docker/CantaloupeOpenJPEG.Dockerfile \
  -t sipi-benchmark/cantaloupe-openjpeg:5.0.7 benchmarks/docker

build/iiif-validator-venv/bin/python benchmarks/test_archive_compare.py
build/iiif-validator-venv/bin/python benchmarks/archive_compare.py \
  --corpus build/archive-images \
  --sample ../OLDAP/oldap-mediaserver/imageserver/cantaloupe-5.0.7/cantaloupe.properties.sample \
  --output build/archive-compare-check-new --check-only

build/iiif-validator-venv/bin/python benchmarks/archive_compare.py \
  --corpus build/archive-images \
  --sample ../OLDAP/oldap-mediaserver/imageserver/cantaloupe-5.0.7/cantaloupe.properties.sample \
  --output build/archive-compare-new
```

Each output directory must be new. Pillow in the external-validator environment
provides independent decoding and LittleCMS colour checks. All four native ARM64
images must already exist locally; their references are explicit in the runner
and resolved to immutable local image IDs before starting. `--servers sipi`
restricts the experiment without changing the workload. Do not run comparisons
concurrently or under intentional host load.

The Cantaloupe Dockerfile installs Ubuntu's `libopenjp2-tools=2.5.0-2ubuntu0.5`
and records actual package versions in `/opt/iiif/benchmark-packages.txt`.
The recorded build uses `libopenjp2-7` at the same version and OpenJDK
`21.0.11+10-1~24.04.2`. The inherited Cantaloupe JAR remains 5.0.7.
The local base used was
`sha256:9ee368f7d0341f315575dab2fcea35f98fc4aabdbda7527fa7625b59f300f671`.
For exact rebuilds, retain that image and pass its ID as `CANTALOUPE_BASE`;
the default local `latest` tag is mutable, and apt availability is external.
This is a local benchmark image, not a deployment or publication artifact.

The runtime config comes from the public sample, with delegates disabled and a
local file source. Cantaloupe uses Java2dProcessor for TIFF and
[OpenJpegProcessor](https://cantaloupe-project.github.io/manual/5.0/processors.html)
for JP2. The latter invokes an external `opj_decompress` process; Java handles
subsequent transforms. `OPJ_NUM_THREADS=1` limits each decoder process. This is a
documented available configuration, not a claim to measure the fastest possible
Cantaloupe installation. SIPI uses Grok; IIPImage and Triplet use OpenJPEG, with
Triplet going through libvips. These are server pipelines, not an isolated codec
comparison with identical implementations.
SIPI runs four image workers with one Grok decoder thread per request; IIP uses
four FastCGI processes with one OpenJPEG/OpenMP thread each. Triplet permits four
transforms with libvips concurrency one and `GOMAXPROCS=4`. Cantaloupe keeps its
HTTP thread pool, Java active processor count four and a 2 GiB heap. A CPU quota
does not make these scheduling architectures identical.

## Requests and limits

Each image contributes equally to five request categories:

| Category | Source region | Output |
| --- | --- | --- |
| Native offset | 256×256 at (17,29) | 256×256 |
| Native edge | Bottom-right 256×256 | 256×256 |
| Fractional square | Centred 513×513 | 256×256 |
| Fractional rectangle | Centred 1023×769 | 384×288 |
| Overview | Complete image | Width 512, proportional height |

The fractional rectangle deliberately uses explicit `w,h`, with a small aspect
ratio change. Overview height permits one pixel of rounding difference. These
requests exercise nonaligned crop boundaries and noninteger reductions. They do
not test rotation, watermarks, upscaling or browser pan/zoom traffic distribution.

Both benchmark source formats are generated losslessly from the normalized TIFF
masters; the originally supplied JP2 files are preserved but are not timed here.
Both source formats contain identical full-resolution pixels and ICC profiles,
but TIFF pyramid averaging and JPEG2000 wavelet levels are different. Lower-scale
outputs need not have identical pixels or equal visual quality.

All requested output axes are below 2048. SIPI uses existing `max-width=2048`,
`max-height=2048`, `max-area=4194304`; Triplet uses the corresponding bounds plus
`max_source_pixels=300000000`; IIP uses `MAX_CVT=2048`. Cantaloupe receives the
same bounded requests. These controls allow SIPI's conservative full/max admission
check to accept the large sources; they do not reduce advertised source dimensions.
They are not equivalent peak-memory limits or a claim that each server implements
identical admission policy. All containers have four CPU cores of quota and 4 GiB
memory, no swap. Full-resolution source coordinates remain unchanged.

## Measurement and correctness

- Two inputs (TIFF/JP2), two outputs (JPEG/PNG), one/eight persistent clients,
  rendering/cache-enabled modes, four servers, three repetitions: 192 timed trials.
- Each workload contains 30 URLs: six images times five categories. **Each client
  completes entire workload rounds**, rotated by client index. Fast images cannot
  gain a larger request share because their client finishes more rounds.
- Fixed-seed shuffled order, serial reference checks, then at least 15 seconds of
  mixed warmup before each server/cache case. Each timed client runs for at least
  five seconds and finishes its current full round. Slow decoders can extend a trial.
- Every serial response is decoded, checked for MIME/dimensions and compared with
  the normalized reference. Native PNG crops must match exactly in common sRGB.
  Raw channel errors are retained separately. Reduced outputs are compared with a
  BOX-filter reference for diagnostics, not required to use that sampling kernel.
- Embedded ICC profiles are honoured. Diagnostic assumptions: untagged RGB is
  sRGB; untagged gray has the normalized source's Gray Gamma 2.2 profile. Cantaloupe
  converts gray JP2 output to sRGB: different raw channel values can be correct.
  Its tested configuration omits embedded profiles in many outputs. Exactness
  under these diagnostic assumptions does not prove that every browser will
  interpret untagged gray identically, or that the pipelines preserve equal metadata.
- Every measured body must match its server's serial MIME and SHA-256; reference
  bytes must also stay identical across cache modes and repetitions. This detects
  errors during measurement without requiring different encoders to match bytes.
- Per-request latency includes complete body receipt, excludes client hashing and
  client queue time. Throughput includes client verification overhead. The saved
  image/category latency statistics are diagnostics, not isolated per-image rates.

The same cache and JPEG caveats as the synthetic comparison apply: IIP's cache
stores native image tiles; the other three use filesystem response caches on fresh
512 MiB tmpfs mounts. This is warm OS/metadata performance, not cold disk or durable
cache-write performance. SIPI JPEG uses quality 90 and 4:4:4; Cantaloupe/IIP use 90
and 4:2:0; Triplet uses stock quality 75 with chroma subsampling. Therefore JPEG
results are not a comparison at equal distortion or equal output size.

`results.json` retains every measured latency/size/path; `summary.json` retains
per-trial metrics, per-image/category latency distributions, references and grouped
medians/ranges. The run directory also keeps source snapshots, container commands,
generated configs and logs. Check `complete: true` before interpreting a run.
Ranges describe repetition spread, not statistical confidence intervals.

No peak-memory or CPU-per-request sampling is performed. An aggregate over this
balanced mix is not a production capacity promise, and it does not isolate the
effect of codecs, colour conversion, resamplers, file layout or server architecture.

## Controlled JPEG backend and sampling experiment

Recorded run: [2026-10-08 JPEG follow-up](benchmarks/2026-10-08-jpeg-backends.md).

Use `--jpeg-comparison` to replace the server list with three SIPI variants:

| Label | Image/backend | JPEG settings |
| --- | --- | --- |
| `ijg-444` | Historical image, IJG 9f | Hardcoded 90 / 4:4:4 |
| `turbo-444` | Current image, libjpeg-turbo 3.2.0 | Explicit 90 / 4:4:4 |
| `turbo-420` | Same current image | Explicit 90 / 4:2:0 |

```sh
build/iiif-validator-venv/bin/python benchmarks/archive_compare.py \
  --corpus build/archive-images \
  --sample ../OLDAP/oldap-mediaserver/imageserver/cantaloupe-5.0.7/cantaloupe.properties.sample \
  --output build/archive-jpeg-new --jpeg-comparison \
  --baseline-image lrosenth/sipi-2:verify-0.1.0-arm64 \
  --turbo-image sipi-benchmark/sipi-2-turbo:verify-0.1.0-arm64
```

The public Cantaloupe sample is retained as runner provenance but is not mounted
or used by the SIPI-only experiment. Image arguments allow retained immutable IDs
instead of mutable tags. The old image must implement its historical 90/4:4:4
defaults; it receives no new JPEG flags. SOF sampling factors are checked from
actual JPEG responses; grayscale must remain a single 1×1 component.

The same TIFF/JP2 × JPEG/PNG × 1/8-client × rendering/cache matrix runs in three
shuffled repetitions: **144 trials**, with unchanged quotas, requests and timing.
PNG provides a control group. All variants run in the same session, including the
old baseline; do not infer improvements by comparing runs from different days.
Source TIFFs use LZW, so these inputs do not exercise JPEG decoding. This measures
the server pipeline with a different JPEG encoder, not an isolated codec kernel.
The historical/current binaries also differ in configuration plumbing and cache
recipe. The two turbo sampling variants use exactly the same binary.

Record actual sampling, output sizes and image-error diagnostics together with
throughput. Quality 90 is a nominal setting, not proof of equal visual fidelity;
4:2:0 discards chroma detail. Do not change production defaults based on throughput
alone. Cached response rates do not measure encoding performance.
