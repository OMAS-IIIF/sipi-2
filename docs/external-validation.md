# External IIIF Image API validation

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

## Method and scope

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
python3 tests/external_iiif_validator.py build/Debug/sipi-2 build/external-image-validator build/iiif-validator-venv/bin/python build/external-validation.json
```

Reuse existing checkout/venv instead of cloning them again. The runner requires a
clean upstream checkout, records installed versions and fails on upstream test
failures or missing test execution. It is opt-in, outside the offline CTest suite;
it does not fetch or install dependencies itself. To reproduce the dependency set
exactly, use the versions retained in the evidence (including Pillow and lxml).
The persistent checkout/venv are local build artifacts, not SIPI runtime dependencies.
