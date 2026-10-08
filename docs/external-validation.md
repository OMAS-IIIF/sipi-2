# External IIIF Image API validation

## Feature-freeze validation — 2026-10-08

The unchanged consortium validator at commit
`f05e365a1cb28f8707cbd50466a86d8f611d1ab8` passed against the current Release binary:

| Service | Level 2 | Selected optional tests | Failures |
| --- | --- | --- | --- |
| Original 1000 × 1000 reference | 31 | 8 | 0 |
| Restricted public 1000 × 1000 canvas from a 2000 × 2000 source | 31 | 8 | 0 |

**78 upstream tests passed.** [Evidence](audits/2026-10-08-external-validator.json)
retains commands, summaries, full upstream output, both policies, source/binary
hashes, Python versions and server logs. The upstream checkout remains clean.
No server implementation or upstream assertion was changed for these runs.

The validator hard-codes a 1000 × 1000 reference pattern. For the restricted test,
the runner creates a 2000 × 2000 source by duplicating each official reference
pixel into a 2 × 2 block (Pillow nearest-neighbor). SIPI must reduce it to a public
1000 × 1000 image. Before validation, Pillow verifies that the public raster's
RGBA pixels exactly equal the untouched official reference. An unprefixed original
alias must return 403. All requests with the public prefix, including nested public
prefixes used by malformed-identifier probes, receive the same restriction.
Unknown allowed public resources therefore reach source lookup and return 404.
This is an isolated test policy; no deployed authorization policy was changed.

### Validator limitations and diagnostic evidence

The [initial diagnostic run](audits/2026-10-08-external-validator-diagnostics.json)
records three failures with explained causes:

- The upstream `format_webp` test failed on both services because it compares
  `img[8:12]` (Python 3 bytes) with `"WEBP"` (text). That comparison is always false.
  It is excluded from the passing upstream selection, not patched or reported as
  an upstream pass. The runner independently verifies HTTP 200, `image/webp`,
  RIFF/WEBP signatures, full Pillow decoding and 1000 × 1000 dimensions for both
  services. These extra checks passed and are separately recorded.
- The first public test policy rejected every nested prefix with 403. Upstream's
  `id_error_escapedslash` expects 404 for its unknown `/public/a/b/...` resource.
  The final isolated policy permits nested public prefixes **with the same
  resolution restriction**, while still denying nonpublic aliases. The unchanged
  level-2 suite then passes. A policy denial is not converted into a server 404.

The eight upstream optional tests cover canonical/profile links, mirroring,
mirroring plus 180° rotation, arbitrary full/region rotation, gray and bitonal.
Upscaling is not enabled in these runs; it remains covered by project tests.
The pinned upstream `size_up` also has fixed sizing assumptions, so this result
must not be presented as external coverage of that feature. Watermarks, S3,
credentialed CORS and real proxy/viewer deployments likewise need their separate
regression/integration checks. This is external validation, not formal certification.

Temporary listeners, repositories and caches were stopped/removed after the runs.
Feature freeze remains active. Next: consolidate documentation/support matrices,
finish the color-management handbook and prepare the release.

## Earlier baseline — 2026-10-06


On 2026-10-06 SIPI-2 passed the unmodified [IIIF consortium image-validator](https://github.com/IIIF/image-validator)
at commit f05e365a1cb28f8707cbd50466a86d8f611d1ab8:

| Selection | Tests | Failures |
| --- | --- | --- |
| Image API 3.0, level 2 | 31 | 0 |
| Implemented optional capabilities | 8 | 0 |

[Full evidence](audits/2026-10-06-external-validator.json) includes verbose upstream
output, validator commit, Python dependencies, source-image SHA-256, server-binary
SHA-256 and temporary server logs. No application code or validator assertions
were changed to obtain this result.

## Earlier baseline method and scope

The upstream command-line validator ran in an isolated Python environment against
a temporary loopback listener with the official, unmodified 1000 by 1000 PNG
reference image from its checkout. A disposable image repository, allow-only Lua
policy and result cache were used. Defaults include wildcard noncredentialed CORS,
no upscaling and the normal output limits. The server was stopped and temporary
repository/cache removed after both runs. Nothing was exposed to the internet.

Optional tests explicitly selected canonical/profile links, mirroring, mirroring
with 180-degree rotation, full/region arbitrary rotation, gray and bitonal. The
validator internally assigns optional tests the number 3; that is not an IIIF
compliance level. We did not select unsupported output formats or upscaling.

The upstream tests include random sizes, positions and angles; exact requests are
retained in the report but subsequent runs sample differently. Some optional
checks are shallow: arbitrary-angle tests mostly verify successful image reads,
and the canonical test checks presence rather than full normalization equivalence.
Our own geometry replay, pixel, policy, CORS and cache tests remain complementary.

This is a successful external-validator run, not certification or proof of every
API/input combination. Other source codecs, S3, restrictive output policies and
credentialed CORS are covered separately by project tests. A real browser/viewer
behind Caddy, HTTPS and cross-origin cookies remains the next deployment check.

## Repeating the check

From the repository root (Python 3 and libmagic required):

```sh
# macOS native dependency, installed once:
brew install libmagic

git clone https://github.com/IIIF/image-validator.git build/external-image-validator
git -C build/external-image-validator checkout f05e365a1cb28f8707cbd50466a86d8f611d1ab8
python3 -m venv build/iiif-validator-venv
build/iiif-validator-venv/bin/pip install ./build/external-image-validator
python3 tests/external_iiif_validator.py build/release/sipi-2 build/external-image-validator build/iiif-validator-venv/bin/python build/external-validation.json
```

The current runner checks both original and genuinely reduced public services,
plus the independent WebP checks described above. Reuse existing checkout/venv
instead of cloning them again. The runner requires a
clean upstream checkout, records installed versions and fails on upstream test
failures or missing test execution. It is opt-in, outside the offline CTest suite;
it does not fetch or install dependencies itself. To reproduce the dependency set
exactly, use the versions retained in the evidence (including Pillow and lxml).
The persistent checkout/venv are local build artifacts, not SIPI runtime dependencies.
