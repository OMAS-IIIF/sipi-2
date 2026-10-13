# SIPI-2 documentation

Start with the [project overview and Docker quickstart](../README.md), then the
[complete feature catalog](features.md) for supported behavior and boundaries.
These guides describe the current implementation; dated audits and benchmark
reports record evidence for a particular revision, platform and workload.

## Setup and operation

| Task | Guide |
| --- | --- |
| Compile, run and test natively; configure CLion | [Native build](building.md) |
| Use container mounts, runtime settings and Compose; build images | [Docker](docker.md), [TOML/Lua example](docker.md#toml-lua-and-a-persistent-cache) and [Compose example](../docker/compose.example.yaml) |
| Configure native operation, paths, CLI/environment/TOML precedence and limits | [Native configuration and option reference](command-line.md), [runnable example](../examples/native/sipi.toml) and [annotated template](../sipi.example.toml) |
| Map public identifiers to image sources | [Source resolution](source-resolution.md) |
| Configure S3-compatible storage | [S3 source adapter and settings](s3-byte-source.md) |
| Set up MinIO for local integration tests | [Development MinIO](minio-development.md) |
| Set external URLs, browser origins and HTTP policy | [IIIF HTTP behavior](iiif-http.md), [information responses](iiif-info.md), [canonical URLs](canonical-uri.md) |
| Bound output sizes and enable enlargement | [Output limits](output-limits.md), [upscaling](upscaling.md) |
| Configure worker and cache capacity | [Worker pool](worker-pool.md), [persistent results](persistent-cache.md), [metadata cache](metadata-cache.md) |

## Image formats and processing

Format support is limited to the documented pixel layouts and color declarations.
A readable header alone does not guarantee pixel delivery. WebP is an output
format; JPEG 2000 and HTJ2K are input formats.

| Topic | Reference |
| --- | --- |
| TIFF tiles, pyramids and geometry | [TIFF metadata/read planning](tiff-metadata-and-geometry.md) |
| TIFF strips and grayscale | [Strip and gray delivery](tiff-strips-and-gray.md) |
| JPEG 2000 and HTJ2K input | [JPEG 2000 reader](jpeg2000-input.md) |
| JPEG input and orientation restrictions | [JPEG reader](jpeg-input.md) |
| PNG input and color declaration restrictions | [PNG reader](png-input.md) |
| JPEG encoding and quality/sampling configuration | [JPEG delivery](jpeg-delivery.md) |
| PNG and WebP output | [PNG delivery](png-delivery.md), [WebP delivery](webp-delivery.md) |
| Transparency and compositing | [Alpha handling](alpha-delivery.md) |
| ICC, sRGB, grayscale and bitonal conversion | [Color management](color-management.md) |
| Reduction and enlargement | [Area resampling](area-resize.md), [upscaling](upscaling.md) |
| Mirroring and rotation | [Orthogonal transforms](orthogonal-transforms.md), [arbitrary angles](arbitrary-rotation.md) |

## Lua policies and integration

Read the input/output contract before adapting the minimal example. Scripts are
trusted operator code and are snapshotted at startup. OLDAP is an optional
integration example; generic delivery does not depend on it.

| Task | Reference |
| --- | --- |
| Write `preflight(request)` allow/deny and source decisions | [Preflight guide and contract](preflight.md), [minimal example](../examples/preflight.lua), [JWT policy](../examples/preflight-jwt.lua) |
| Supply settings and secrets | [Lua configuration](lua-configuration.md) |
| Parse and generate JSON | [JSON helpers](lua-json.md) |
| Call an external HTTP(S) API | [HTTP helpers](lua-http.md) |
| Sign/verify tokens with explicit claim policy | [JWT helpers](lua-jwt.md) |
| Share bounded cached policy values | [Lua value cache](lua-cache.md) |
| Expose a restricted public image | [Restricted resolution](restricted-resolution.md) |
| Apply a required watermark | [Watermarks and mask cache](watermarks.md) |
| Integrate with OLDAP | [OLDAP policy](oldap-preflight.md) |

## Architecture and development

- [Delivery architecture and boundaries](iiif-delivery-architecture.md)
- [HTTP integration](http-server.md), [IIIF parser](iiif-parser.md), and [byte sources](byte-source.md)
- [Encoded-result cache identities](cache-key.md) and [persistent cache lifecycle](persistent-cache.md)
- [Development/test/release commands](makefile.md) and [dependency updates](dependency-updates.md)
- [Embedded ICC profile provenance](../third_party/compact-icc/README.md)

## Validation and performance

- [External IIIF validation: scope, limitations and reproduction](external-validation.md)
- [IIIF conformance audit history](iiif-conformance.md): later results supersede earlier findings
- [Synthetic workloads and worker benchmarks](benchmarks.md)
- [Server comparison methodology](server-comparison.md)
- [Real-image corpus preparation and provenance](archive-test-corpus.md)
- [Archive comparison methodology](archive-comparison.md)
- [JPEG backend comparison](benchmarks/2026-10-08-jpeg-backends.md)
- [Resampling comparison](benchmarks/2026-10-12-axis-weights.md)
- [HTJ2K comparison](benchmarks/2026-10-12-htj2k.md)
- [Portable resize benchmark](benchmarks/resize-docker.md)

Benchmark reports qualify hardware, sources, encoder settings, cache behavior and
measured scope. They are not universal speed rankings. Some archive measurements
require private originals; use the [synthetic quickstart image](../examples/images/README.md)
for onboarding instead. Raw JSON evidence accompanies reports under `benchmarks/`
and `audits/` within this documentation directory.
