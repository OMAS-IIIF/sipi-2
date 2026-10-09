# Real-image server comparison — 2026-10-08

The completed run covers six normalized archival/cartographic images, paired as
lossless pyramidal tiled TIFF and JP2. It includes a 229 MP map. All four servers
completed **192 timed trials and 1,201,230 byte-verified responses**.
Serial validation decoded 2,880 outputs and checked 288 corpus info documents;
576 native PNG crops match in common sRGB under the documented ICC assumptions.
Three benchmark accounting/geometry regressions also pass.

See [reproduction and methodology](../archive-comparison.md), the
[prepared corpus](../archive-test-corpus.md), and [machine-readable results](2026-10-08-archive-servers.json).
The separate [synthetic TIFF baseline](2026-10-08-servers.md) uses another workload;
do not compare its absolute rates with these as if only the input image changed.

## Main observations

- Pyramidal TIFF is faster than JP2 in this **balanced mixed rendering workload**
  on each server, for both output formats. For SIPI JPEG output the ratio is
  7.2×. This is not a claim about every individual request.
- SIPI JPEG rendering is 1.76× the tested Cantaloupe rate from
  TIFF and 1.91× from JP2. The configured JPEG encoders/subsampling
  and colour pipelines differ, so these are operational rates, not equal-quality
  encoder measurements. Cantaloupe's JP2 path includes an external OpenJPEG process.
- IIPImage has higher TIFF rendering throughput here. SIPI and IIPImage JP2
  rendering are much closer; use the ranges rather than declaring a universal winner.
- Response caching largely removes decoding differences for SIPI, Cantaloupe and
  Triplet. IIP's native tile cache is a different operation and shows little gain
  for these JP2 requests. No external HTTP response cache was added to IIP.
- There are request-specific reversals: Triplet's map overview is faster from JP2
  than TIFF. The aggregate mix must not conceal this case.

## Throughput with eight concurrent clients

Requests/second: **median [minimum–maximum]** of three trials. Each client completes
whole rounds of six images × five request categories. One-client results and
latency statistics remain in the JSON; ranges are not confidence intervals.

### Rendering, derivative/tile retention disabled

| Server | TIFF → JPEG | JP2 → JPEG | TIFF → PNG | JP2 → PNG |
| --- | ---: | ---: | ---: | ---: |
| SIPI-2 | 476.4 [453.0–485.6] | 65.8 [64.5–66.3] | 192.3 [184.6–195.4] | 56.4 [56.3–56.7] |
| Cantaloupe 5.0.7 | 270.5 [268.0–273.6] | 34.4 [34.3–34.8] | 205.7 [199.5–210.6] | 33.1 [32.3–33.2] |
| IIPImage 1.3 | 782.5 [774.5–787.9] | 63.4 [61.1–63.9] | 466.9 [450.4–469.0] | 59.5 [53.8–60.7] |
| Triplet | 81.0 [79.7–82.4] | 20.8 [20.8–21.0] | 78.0 [72.8–80.4] | 20.3 [20.3–20.6] |

OS and metadata caches are warm; this is not a cold-storage measurement.

### Cache enabled and populated

| Server | TIFF → JPEG | JP2 → JPEG | TIFF → PNG | JP2 → PNG |
| --- | ---: | ---: | ---: | ---: |
| SIPI-2 | 6140.1 [5786.2–6367.3] | 5932.2 [5739.4–6085.8] | 2336.1 [2227.5–2352.2] | 2244.2 [2191.1–2277.0] |
| Cantaloupe 5.0.7 | 6158.7 [6095.6–6259.2] | 6280.7 [6163.3–6365.2] | 1836.4 [1766.6–1907.0] | 1750.3 [1624.9–1759.4] |
| IIPImage 1.3 | 2490.6 [2486.0–2542.3] | 63.9 [62.8–65.4] | 777.9 [757.4–795.6] | 58.8 [57.3–60.0] |
| Triplet | 8668.3 [7959.8–8924.8] | 7882.4 [7511.2–8686.7] | 1770.3 [1742.1–1938.0] | 1830.0 [1331.6–1836.0] |

**These caches are not equivalent:** SIPI/Cantaloupe/Triplet retain encoded
responses on 512 MiB tmpfs; IIP uses a native image-tile RAM cache (128 MiB per
FastCGI worker, four workers). The table does not compare durable cache writes,
eviction, restarts or a common reverse-proxy cache. Client verification and Docker
loopback transfer also limit these high rates.

## One illustrative image: the 229 MP map

JPEG request latency in milliseconds, median of the three per-trial medians with
**one client**, retention disabled. These are observations within the balanced
workload, not separate dedicated tests of these URLs. Native offset is the
256×256 crop at (17,29); overview has width 512 and proportional height.

| Server | TIFF native crop | JP2 native crop | TIFF overview | JP2 overview |
| --- | ---: | ---: | ---: | ---: |
| SIPI-2 | 2.4 | 22.5 | 9.2 | 171.0 |
| Cantaloupe 5.0.7 | 3.4 | 84.9 | 11.4 | 158.9 |
| IIPImage 1.3 | 1.5 | 69.5 | 4.4 | 158.6 |
| Triplet | 4.2 | 141.8 | 552.6 | 148.8 |

Other image/category timings and sample counts are retained in every trial's
`by_request` mapping. Do not infer the cause of a slow request from latency alone;
no CPU stack or I/O profile was captured here.

## Output and colour differences

Mean encoded body size, KiB, over the 30 equally weighted URLs in each workload:

| Server | TIFF → JPEG | JP2 → JPEG | TIFF → PNG | JP2 → PNG |
| --- | ---: | ---: | ---: | ---: |
| SIPI-2 | 44.4 | 47.4 | 151.3 | 160.2 |
| Cantaloupe 5.0.7 | 33.0 | 32.0 | 185.3 | 195.2 |
| IIPImage 1.3 | 39.3 | 42.2 | 164.9 | 174.7 |
| Triplet | 26.1 | 27.1 | 196.0 | 199.9 |

SIPI uses JPEG quality 90 with 4:4:4 RGB sampling; Cantaloupe/IIP use 90 and 4:2:0;
Triplet uses stock quality 75 and 4:2:0. Header sampling was independently checked.
Gray JPEG output has one component, except Cantaloupe's JP2 gray-to-sRGB path.
PNG also trades compression time against size, and reduced pixels differ between
resamplers and between TIFF averaging pyramids and JPEG2000 wavelet levels.

SIPI, IIP and Triplet embed ICC profiles in all 120 reference outputs per server.
Cantaloupe embeds them in only 10 here (native RGB TIFF-to-JPEG crops). Its gray
JP2 is converted to RGB sRGB, while its gray TIFF output retains raw gray samples
without an ICC profile. Hence two native JP2 PNG crops differ in raw channel values
but match the colour-managed reference. The diagnostic fallback assigns normalized
Gray Gamma 2.2 to untagged gray and sRGB to untagged RGB. This verifies this corpus
under stated assumptions; it does not prove equal metadata preservation or that
all viewers interpret untagged gray identically. No competitor was patched.

## Evidence, controls and limits

- Run: `2026-10-08T13:33:17Z` to `2026-10-08T14:04:22Z`.
- Apple Silicon host; native Linux ARM64 containers, four-CPU quota, 4 GiB memory,
  no swap; only one benchmark server active at a time. Existing OLDAP/MinIO
  services remained untouched. Host background load is not fully controlled.
- Explicit 2048px output bounds for SIPI/Triplet/IIP; identical small output
  requests for every server. These admit large sources in SIPI without reducing
  source coordinates. They are not identical server memory/admission policies.
- TIFF and JP2 benchmark files are generated losslessly from normalized TIFF
  masters; original downloaded JP2s are not used in this comparison. All prepared
  input SHA-256 values were checked before and after; source files are unchanged.
- All trials contain all 30 paths with equal counts. References retain identical
  response hashes across all repetitions and cache modes; every measured response
  matches its own server's reference MIME/hash. Comparisons between encoders do
  not require identical JPEG bytes or identical reduced pixels.
- Exact image IDs, Docker facts, corpus/sample/runner hashes and grouped/per-trial
  statistics are in the JSON. Raw timings, response hashes, source snapshots,
  container commands, generated configs and logs remain in
  `build/archive-compare-2026-10-08/`. The JSON records the raw results SHA-256.
- No peak-memory, CPU-per-request, cold disk, S3, TLS, authorization, watermark,
  rotation, failure-load or sustained deployment test is included. Six selected
  images and five fixed operations are not a production capacity prediction.

## Next useful decision

Use pyramidal tiled TIFF as the evidence-supported delivery default for this
workload, retaining JP2 support and its storage advantages for most photographic
sources. Before any further tuning, decide whether to compare JPEG encoders at
closer quality/subsampling settings. The already proposed server-wide JPEG quality
setting and libjpeg-turbo evaluation remain separate, unimplemented follow-ups;
the delivery feature freeze and existing deployments are unchanged.
