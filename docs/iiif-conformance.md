# IIIF Image API 3 conformance audit — 2026-10-06

## Feature-freeze follow-up (2026-10-08)

The unchanged consortium validator now passes 31 level-2 and eight selected
optional tests on **each** of the original and restricted public services (78/78).
The public canvas genuinely reduces a 2000 × 2000 source to the required 1000 ×
1000 reference; exact reference pixels are checked before validation. Independent
WebP signature/full-decode checks also pass. The upstream Python-3 WebP bytes/text
bug and the initial test-policy 403/404 mismatch are preserved and explained in
[method and limitations](external-validation.md), with
[current evidence](audits/2026-10-08-external-validator.json) and initial diagnostics.
No production code or upstream assertions changed. Feature freeze continues with
documentation and release preparation; this is not formal certification.

## External validation completed

The unchanged consortium validator (commit f05e365a1cb28f8707cbd50466a86d8f611d1ab8)
passed all 31 Image API 3.0 level2 tests and eight selected implemented optional
tests on 2026-10-06. See [method, scope and reproduction](external-validation.md)
and [raw evidence](audits/2026-10-06-external-validator.json). This supersedes the
pending-external-validator notes below. Browser/Caddy/cookie deployment checks
remain open; the pass is not a formal certification.

## Latest follow-up: canonical links

[Canonical URI generation](canonical-uri.md) is now a separate pure function;
image responses deliver pixels directly and include canonical plus profile links,
including cache hits and HEAD. Per-resource info advertises canonicalLinkHeader.
[Latest evidence](audits/2026-10-06-canonical.json): 41/41 probes pass. External
validator and deployed browser/Caddy testing remain open. Earlier snapshots below
are retained as history, not a description of current missing functionality.

## Current follow-up: HTTP and profile

Resource admission and required HTTP behavior are implemented. Admitted resources
publish level2, additional qualities and accurate optional features. Redirects,
JSON/JSON-LD negotiation, configurable wildcard/credential-aware CORS, OPTIONS,
profile links and unsupported-format 400 responses have regression coverage.
[Updated evidence](audits/2026-10-06-http.json): 40/41 probes pass; the remaining
probe is optional canonical links. This is not external certification. External
validator, viewer and Caddy deployment checks remain future work.
See [HTTP contract](iiif-http.md). Findings below are the original historical audit.

## Earlier follow-up: resource admission and limits

Implemented after this audit: public maxWidth/maxHeight/maxArea configuration,
shared info/request policy, metadata-only source rejection and full/max budget
admission. The wide-JPEG and unsupported-orientation findings are resolved.
See [the implementation contract](output-limits.md). The evidence below remains
the original audit snapshot; HTTP/profile findings were subsequently addressed above.

## Original audit conclusion

Do not publish a blanket level2 profile yet. The renderer covers the tested
Level 2 image operations, but HTTP behavior and resource eligibility need work.
This audit changes no runtime behavior; the approved temporary profile omission
remains visible, not reclassified as conformance.

References: [Image API 3.0](https://iiif.io/api/image/3.0/) and its
[compliance matrix](https://iiif.io/api/image/3.0/compliance/).
Level 2 requires percentage regions/sizes, confined sizing, quarter turns,
JPEG/PNG and applicable color/gray quality, alongside lower-level operations.
Levels 1 and 2 require base redirects, CORS and JSON-LD media type.
Upscaling, arbitrary rotation, mirroring, bitonal and canonical/profile links
are optional at every level.

## Reproduction

Run from the repository root after building:

    python3 tests/iiif_conformance_audit.py build/Debug/sipi-2

The script owns a temporary server and generated PNG/TIFF fixtures. It prints
JSON and exits 1 while findings remain. It stays outside CTest rather than
asserting known defects as desired behavior. It is not an official validator.
[Saved evidence](audits/iiif-3-2026-10-06.json) normalizes ephemeral ports.

27 of 41 probes pass. The remaining 14 include repeated CORS probes and optional
features; this is not a percentage conformance score. All 43 existing CTest
suites pass, including MinIO; regression tests preserve development boundaries
and cannot establish conformance by themselves.

## Findings

| Priority | Observed behavior | Code / consequence |
| --- | --- | --- |
| High | Every info document omits profile | IiifInfo.cpp; required declaration is absent. |
| High | Orientation-6 TIFF: info 200, image 501 | IiifWorker builds info even when readable is false. Header reading is not image delivery. |
| High | 70000 × 1 PNG: info 200 without limits; full/max/default.jpg returns 500 encoding_failed | JPEG dimension limit is checked late; advertised dimensions and available output diverge. |
| High | /project/color.png returns 400 | Parser only accepts image/info syntax; no base redirect. |
| High | No CORS on info, image or error; OPTIONS returns 405 | HttpServer's read_only_methods blocks browser preflight. |
| High | Accept application/ld+json receives application/json | No content negotiation. |
| Medium | Color sources announce gray/bitonal only; PNG is not discoverable through profile/extraFormats | Info under-describes working capabilities. |
| Medium | Unsupported TIFF output returns 501 | Format-specific rule recommends 400; split this from unsupported operations. |
| Optional | No canonical/profile Link headers | Add once canonical identity and actual profile are defined. |

The specification requires profile and additional-quality declarations.
Unsupported format has a specific 400 recommendation. Output limits can use
maxWidth/maxHeight/maxArea. These rules are in the
[technical properties and response sections](https://iiif.io/api/image/3.0/).
The metadata-only issue is an architectural finding: adding a profile cannot
make that resource's missing pixel delivery compliant.

## Working paths and limits of the audit

Live probes passed full/pixel/percentage/square regions, clipping, all required
downscaling forms including aspect distortion, quarter turns, mirroring, and
all four qualities in JPEG/PNG. HEAD preserved length without a body; prefixes
survived; outside regions returned 400 and disabled enlargement 501.
16-bit PNG was already rejected at metadata access.

Existing tests also exercise arbitrary rotation, ICC, cache authorization/restart,
optimized TIFF/J2K and local/S3 parity. The focused audit does not independently
prove every combination, corrupt input, browser, credentials or proxy behavior.

## Implementation sequence

1. Define eligible resources and output limits. Reject unsupported source layouts
   consistently at info and image endpoints; keep any future metadata-inspection
   endpoint separate. Share capabilities and deterministic limits between info
   and geometry. Existing format-neutral SizePolicy is a starting point.
   Decoder/LCMS/scratch memory budgets are not simply an output maxArea.
2. Implement HTTP requirements. Redirects retain encoded prefix, configured public
   origin and authorization/source resolution. Negotiate JSON/JSON-LD including
   q-values and parameters, with Vary: Accept. Handle OPTIONS separately from
   image authorization and apply CORS to success/error/redirect responses.
3. Publish accurate per-resource capabilities and highest fulfilled profile.
   Target level2 after the above; do not use level0 to hide eligibility problems.
   Add color for color sources; only additional formats/features belong in extras.
4. Add optional canonical/profile links using authorized public identity and
   resolved geometry, never storage paths or untrusted proxy headers.
5. Turn resolved findings into regression tests; then run an external validator
   and browser viewer against the actual proxy deployment.

Two concrete decisions precede implementation:

- CORS: anonymous public access can use wildcard origin. Cross-origin cookie
  credentials require an explicit origin policy, credential headers and
  Vary: Origin. Never combine unrestricted origin reflection with credentials.
  Existing Lua cookie/Authorization support makes this a real deployment choice.
- Output limits: a common cap for JPEG/PNG is the simplest predictable contract.
  Keep deterministic size rejection distinct from corrupt-source or unexpected
  memory failures. New input conversions are not needed merely for Level 2.

## Separate work

Upscaling was subsequently implemented behind size-upscaling=true; see
[implementation and tests](upscaling.md). Advertisement follows this setting. Sizes/tiles/preferred formats, IIIF Auth and
new codecs remain independent improvements. Floyd–Steinberg's per-request pattern
boundaries and JPEG's nonbinary decoded tones remain documented quality limits.
Canonical URLs, internal result cache identity and external cache policy are
different concerns.
