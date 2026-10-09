# Controlled JPEG backend and sampling comparison — 2026-10-08

Three SIPI variants, six identical real-image TIFF/JP2 pairs, quality 90 throughout.
The IJG baseline was rerun in the same session; these are not ratios against earlier timings.
See [method and reproduction](../archive-comparison.md#controlled-jpeg-backend-and-sampling-experiment)
and [complete compact evidence](2026-10-08-jpeg-backends.json). Raw logs, commands, reference hashes and per-response timings remain in `build/archive-jpeg-2026-10-08`.

## Rendering throughput

Eight concurrent clients, response cache disabled. Requests/s: median (minimum–maximum) of three repetitions.

| Variant | TIFF → JPEG | JP2 → JPEG | TIFF → PNG control | JP2 → PNG control |
| --- | ---: | ---: | ---: | ---: |
| ijg-444 | 485.3 (479.2–487.5) | 68.4 (65.6–68.6) | 194.1 (181.0–194.3) | 57.4 (56.4–58.0) |
| turbo-444 | 643.0 (630.4–647.2) | 69.0 (66.1–71.9) | 189.8 (176.1–193.5) | 56.7 (56.6–57.3) |
| turbo-420 | 657.2 (655.4–674.8) | 69.4 (69.2–70.4) | 190.4 (184.6–194.2) | 56.7 (56.1–57.1) |

Relative median JPEG throughput:

- TIF: turbo 4:4:4 / IJG 4:4:4 = **1.32×**; turbo 4:2:0 / turbo 4:4:4 = **1.02×**; combined = **1.35×**.
- JP2: turbo 4:4:4 / IJG 4:4:4 = **1.01×**; turbo 4:2:0 / turbo 4:4:4 = **1.01×**; combined = **1.02×**.

Interpretation: the TIFF → JPEG gain is clear in this workload (about 33% at unchanged 4:4:4, with nonoverlapping observed ranges). JP2 → JPEG medians differ by less than 1% between IJG and turbo 4:4:4, with overlapping ranges; there is no convincing end-to-end gain there. Switching turbo to 4:2:0 adds only about 2% TIFF throughput but reduces mean JPEG bytes by about 15%. PNG controls have identical bytes and overlapping timing ranges. Keep the 90 / 4:4:4 default; choose 4:2:0 for a deliberate bandwidth/colour-detail tradeoff.

## JPEG response sizes

Equally weighted mean over 30 distinct requests per input format; includes five grayscale requests.

| Variant | TIFF source: mean KiB | JP2 source: mean KiB |
| --- | ---: | ---: |
| ijg-444 | 44.4 | 47.4 |
| turbo-444 | 44.4 | 47.4 |
| turbo-420 | 38.0 | 40.2 |

## Single-client rendering and cached responses

| Variant | TIFF JPEG: 1 client | JP2 JPEG: 1 client | TIFF JPEG: 8 clients, cached | JP2 JPEG: 8 clients, cached |
| --- | ---: | ---: | ---: | ---: |
| ijg-444 | 177.8 (171.9–178.7) | 19.9 (19.5–20.0) | 6338.6 (6049.2–6509.4) | 6181.7 (5814.3–6229.6) |
| turbo-444 | 253.2 (251.0–273.4) | 20.7 (20.4–20.7) | 6034.9 (5759.7–6398.8) | 5691.3 (4356.3–6208.9) |
| turbo-420 | 263.0 (256.4–264.7) | 20.6 (20.2–20.7) | 6769.3 (6432.0–7138.4) | 6569.6 (6372.3–6740.8) |

## Validation and limits

- 144 complete trials, 1,092,270 verified measured responses, 2,160 decoded references, 216 info checks and 432 exact native PNG checks.
- IJG and turbo 4:4:4 produced byte-identical JPEGs for 60 / 60 distinct JPEG requests in this corpus. This is observed fixture equivalence, not a general codec guarantee.
- All response hashes are stable within each variant across cache modes and repetitions. PNG bytes are identical across all three variants; grayscale JPEG bytes are identical between the two turbo sampling settings. ICC profiles are unchanged. Actual JPEG SOF factors match the intended sampling.
- Native ARM64 Docker on Apple M5 Max (18 logical CPUs, 36 GiB RAM), four CPU cores of quota / 4 GiB per container, one active benchmark container, warm OS/metadata, 512 MiB tmpfs response caches. No cold-storage, S3, authorization-service or production network latency.
- Fixed shuffled order, three repetitions, at least 15 seconds mixed warmup and five seconds per timed client with complete balanced URL rounds. Ranges are observed spread, not confidence intervals. Host/client scheduling and Docker transport can limit rates.
- Source TIFFs are LZW. This tests the delivery pipeline and JPEG encoding; it does not measure JPEG input decoding or an isolated encoder kernel. The older baseline also predates JPEG config plumbing/cache recipe v4; only the two turbo sampling variants share the exact same binary.
- Equal quality numbers do not guarantee equal distortion between codecs. 4:2:0 deliberately reduces colour detail. Raw/common-sRGB error diagnostics are retained in JSON; BOX reference reductions are not a perceptual quality metric. Cached rates measure serving already encoded bodies, not encoder speed.
- Cantaloupe/IIPImage/Triplet were not rerun in this controlled experiment. Their earlier results remain a separate experiment with different encoder/colour behavior; this report does not establish a new universal server ranking.
- Defaults remain quality 90 / 4:4:4. No product change, registry push or deployment was performed.

## Provenance

Run: 2026-10-08T14:45:12Z to 2026-10-08T15:30:52Z.

| Variant | Immutable local Docker image ID |
| --- | --- |
| ijg-444 | `sha256:00407a2a01712b67f2ded74d5233d43ed9f521cd9de16a10b88f50a46b6ab5bd` |
| turbo-444 | `sha256:ce7b00e89f5faecc7d5776b699aac9c23791cb5f0bfb06a9c1d394983bf09361` |
| turbo-420 | `sha256:ce7b00e89f5faecc7d5776b699aac9c23791cb5f0bfb06a9c1d394983bf09361` |

Corpus manifest SHA-256: `fcbdd69fe385bbe5b8ad7ece19fda383251f21b24179660d948a89e5fb3baf70`.
Raw results SHA-256: `e813a99520907747ab88653a6b8b6e2d7add19a3de2c2f1dbbb5ca0e3fba34a2`.

The JSON retains runner hashes, Docker/host details, variant settings, per-trial/request statistics and all unique reference diagnostics. Prepared source hashes were verified before and after the run; external originals are not redistributed.
