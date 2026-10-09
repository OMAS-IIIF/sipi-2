# Comparing IIIF servers

`benchmarks/server_compare.py` is an opt-in local Docker benchmark. It owns only
uniquely named temporary containers and never replaces the OLDAP deployment.
The first measured scope is aligned 256×256 viewer tiles from the same synthetic
4096×4096 pyramidal TIFF. It is not a general ranking of IIIF servers.

## Reproduce

Use the fixture generator and Release Docker image described in
[benchmarks](benchmarks.md) and [Makefile commands](makefile.md). Pillow is needed
by the client for independent pixel validation; the external-validator virtual
environment already provides it. The client is otherwise Python standard library.
The current runner deliberately requires native ARM64 images, matching this
Apple Silicon test machine; it refuses an emulated AMD64 image.

```sh
build/iiif-validator-venv/bin/python benchmarks/server_compare.py \
  --images build/benchmark-images \
  --sample ../OLDAP/oldap-mediaserver/imageserver/cantaloupe-5.0.7/cantaloupe.properties.sample \
  --output build/server-compare-new \
  --sipi-image lrosenth/sipi-2:verify-0.1.0-arm64 \
  --cantaloupe-image lrosenth/oldap-imageserver:latest \
  --iip-image iipsrv/iipsrv@sha256:11cedb79da309ef0b766c2fadddb5eab6b9bb3a4eb24b49ab941a431d78c9a9c \
  --triplet-image ghcr.io/libops/triplet@sha256:3dd347c3f7e9021c7c32b995f2417a294c881436c497f6b264496f3d8ea65dbf
```

Pull the two public images first with `docker pull --platform linux/arm64 IMAGE`.
Cantaloupe uses the existing local image with its bundled 5.0.7 JAR, but mounts a
fresh configuration derived from the upstream sample. No OLDAP delegates,
credentials, URLs or authorization rules are read by the benchmark. The sample
can be supplied from any local Cantaloupe 5.0.7 distribution. IIPImage and Triplet
are optional; omitting those arguments runs only SIPI-2 and Cantaloupe.

The output directory must not exist. Existing image tags are resolved to immutable
local IDs at startup. The report records those IDs, source/configuration/runner
hashes, platform details, all timings and completion status. Every trial retains
its exact container command, generated configuration, serial reference hashes,
pixel differences and server logs. The runner also writes `summary.json` with
trial aggregates/ranges and reference diagnostics, and retains its own source
as `runner.py`. Preserve this directory for raw evidence.
The checked-in dated report contains aggregates and reference diagnostics.

The separate [real-image comparison](archive-comparison.md) uses six prepared
TIFF/JP2 pairs, including a 229 MP map, variable source dimensions, nonaligned
crops and explicitly configured JPEG2000 decoders. The runner on this page remains
the reproducible synthetic 4096px baseline.

## Workload and controls

- The same read-only media bind mount, one active benchmark server at a time,
  native Linux ARM64, four-CPU quota and 4 GiB container memory limit; swap disabled.
- 128 unique regions per format: 64 native-resolution tiles and 64 tiles reduced
  by two, aligned with the TIFF's main-IFD pyramid. JPEG and RGB PNG responses.
- One and eight persistent HTTP clients on host loopback through Docker Desktop.
  Clients issue the next request after receiving and verifying the previous one.
  No browser, Caddy, TLS, JWT, Lua, remote authorization or S3 in this comparison.
- Three complete repetitions, fixed-seed shuffled server/cache and format/client
  order. Every URL is fetched serially first. Then at least 15 seconds of mixed
  concurrent warmup (including Java JIT) precede measurements.
- Each timed client loops over its assigned URLs for at least five seconds,
  finishing its full round. The initial round has 512 requests for rendering,
  2,048 for cache-enabled trials, divided among clients. Slow phases can run longer.
- Request latency includes receipt of the complete body, excludes local hashing
  and client queue time. Throughput includes verification and client overhead.
  Medians and p95 are computed per trial; the report takes medians of trial
  statistics and retains their ranges. These are not confidence intervals.
- OS caches and metadata are warm. “Rendering” disables derivative/tile retention;
  it is **not** a cold disk or cold process test. Cache-fill latency is not measured.
- SIPI-2: four image workers. Triplet: four transforms, one libvips thread per job,
  `GOMAXPROCS=4`, libvips operation cache disabled. Cantaloupe: Java2dProcessor,
  Java active processor count four, 2 GiB maximum Java heap, normal HTTP thread
  pool. IIPImage: four FastCGI processes behind the image's Lighttpd; one OpenMP
  thread per process. Runtime architectures differ even with equal CPU quotas.
- Health checks and request access logs disabled. Existing unrelated Docker
  services remain running. Host background work and CPU scheduling can interfere.

## Different caches and encoders

SIPI-2, Cantaloupe and Triplet use a fresh 512 MiB tmpfs for their filesystem
response caches. All output URLs are populated before timing. This isolates
response-serving overhead and deliberately excludes persistent disk durability
costs; it does not benchmark production cache writes or restart recovery.
Cantaloupe also resolves its source before a derivative hit. Metadata caches stay
on. IIPImage instead uses its native RAM tile cache (128 MiB per process, four
processes), with no added reverse-proxy response cache. Its “cache on” numbers
must not be described as the same response-cache operation as the other servers.

JPEG is an operational comparison, **not equal distortion**: SIPI-2 uses quality
90 and 4:4:4, Cantaloupe quality 90 and 4:2:0, IIPImage quality 90 and 4:2:0, and the pinned
Triplet image uses its stock quality 75 with chroma subsampling. Sampling factors
are inspected in decoded JPEG headers, and per-output size and pixel errors are
retained. Source inspection of Triplet's file and buffer encoders explains its
fixed setting; the competitor is not patched to improve or worsen its results.

PNG separates lossy JPEG settings from processing, but lossless compression also
trades CPU for output size. SIPI-2, Cantaloupe and IIPImage PNGs must match the
selected TIFF pyramid pixels exactly. All four servers must match native 1:1
crops. Triplet's 1:2 TIFF path resamples the main image instead of selecting the
precomputed level; those differences are recorded, not treated as a conformance
failure. The exception is limited to those Triplet outputs. Exact sampling kernels
are not prescribed by IIIF. This prevents an incorrect claim of identical PNG
pixels across all four servers at reduced resolution.

Every timed response must retain its own server's serial byte hash and MIME type.
All serial responses are fully decoded, dimensions checked, and compared with
source crops. Hash equality across different JPEG encoders is not required.

## Interpretation and next steps

Use the [first results](benchmarks/2026-10-08-servers.md) as a baseline, not a
capacity promise or a comprehensive product comparison. One synthetic RGB8 image,
two exact scales and aligned crops cannot represent photographic/archival masters,
very large scans, nonaligned crops, arbitrary scaling/rotation, watermarking or
multiple-image pressure. No failure-load or overload behavior is measured.

The [archive comparison](archive-comparison.md) implements the real-image and
JPEG2000 extension. Its benchmark-only Cantaloupe image adds OpenJPEG and an
explicit processor configuration; the original image used above has no configured
JPEG2000 processor. Never compare an unsupported path or silently use another
source format for only one server.
Peak memory/CPU-per-request and sustained deployment load need dedicated sampling;
the retained end-of-trial `docker stats` is only diagnostic, not peak RSS evidence.

References: [Cantaloupe processors](https://cantaloupe-project.github.io/manual/5.0/processors.html),
[IIPImage Docker image](https://hub.docker.com/r/iipsrv/iipsrv),
[IIPImage server configuration](https://iipimage.sourceforge.io/documentation/server),
[Triplet source](https://github.com/libops/triplet/tree/b26deb3e698c926291c005e166cec5dfd5095bcc).
