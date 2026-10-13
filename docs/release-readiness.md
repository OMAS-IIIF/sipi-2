# Public release readiness review

Reviewed 2026-10-13 against the working tree based on `dd8675e` and the pending
operations documentation. This is a release-preparation assessment, not a release
announcement or a declaration that all gates pass. No visibility, source tag,
registry tag or repository setting was changed.

## Current release target

The agreed release is **1.0.0**, with planned Git tag `v1.0.0` and Docker image
`lrosenth/sipi-2:1.0.0`. CMake and current usage examples now target this version;
[release notes](../CHANGELOG.md) describe scope and compatibility. No tag or image
has been published by this preparation step. Historical evidence below retains
its original version numbers.

The clean pre-bump revision `cffd74d` matched remote main and passed all ten
workflow regression cases and both container smoke suites (ARM64 native, AMD64
emulated). OCI identity and all 410 Git archive files were verified. Evidence is
retained locally in sibling `sipi-2-release-check-state/clean-release-results.json`.
The final committed 1.0.0 snapshot and published artifacts still need verification.

## Completed foundations

README/navigation, feature catalog, native build/configuration, Docker, Lua policy
and operations guides are present. Runnable examples use an original procedural
PNG. Configuration and policy examples have focused native/runtime evidence;
the published ARM64 image and an isolated Caddy proxy were exercised. See the
[release work record](public-release-plan.md) for exact counts and limitations.
The root [project context](../codex.md) is consolidated; its previous chronology
is preserved in a clearly labelled [archive](history/codex-before-consolidation-2026-10-13.md).

Both excluded third-party originals are ignored, are absent from tracked files,
and have no path entries in locally reachable Git history. Seven owner-supplied
images remain tracked. The earlier approved history rewrite is recorded separately;
this check does not prove removal from GitHub-retained objects or other clones.
Selected logos and their design explorations are tracked; they are no longer
untracked assets awaiting initial inclusion as described in the original inventory.

## Remaining gates and concrete completion criteria

| Area | Evidence / gap | Completion criterion | Type |
| --- | --- | --- | --- |
| Fresh checkout | Passed for macOS ARM64 Debug at `54f8e54`: isolated checkout/Conan cache, 69 CTests and 92 documented-example checks | Native gate complete; subsequent S3/external-validator/ARM64-container follow-up also passed below; other release platforms remain open | Verified, scoped |
| Release provenance | Helper now emits/verifies OCI labels, rejects dirty publication, freezes Git input and records Buildx digests; local ARM64 development run and mocked release/failure tests pass | Commit and verify the final 1.0.0 snapshot; create the planned v1.0.0 tag when publishing and retain the published digest. Old 0.1.3 image identity remains unresolved | Workflow implemented; final release decision open |
| Platform acceptance | Both local source images at 54f8e54 pass build/smoke: ARM64 native, AMD64 emulated on ARM64 | Functional container gate complete for these artifacts; native x86 execution/performance and final published-artifact verification are not implied | Verified, scoped |
| Image/brand attribution | Seven photograph files now explicitly CC BY 4.0 with Dölf/Lukas Rosenthaler credits and confirmed authority; logo usage terms separate | Completed for listed assets; keep the two excluded originals/derivatives private | Documented |
| Code/dependency notices | AGPL-3.0-only software declaration; CC BY documentation; installed-graph notice collector includes Conan/ICC and pinned embedded/AWS/XZ notices | Run make source-package with the final image report; retain, publish and link the resulting sources/recipes/patches to release digests | Notices and source packaging implemented; final artifact publication open |
| Contributions and support | No CONTRIBUTING.md or issue/PR templates; public docs explain builds but not contribution routing or expectations | Provide contribution/support entry point with scoped changes, test tiers and policy for reports; confirm the actual channel before publishing it | Documentation and owner decision |
| Vulnerability reports | No SECURITY.md or confirmed private reporting channel/support window | Owner selects an available private channel and supported-release policy; document it without inventing an email or promising an SLA | Owner decision |
| Release notes | Draft 1.0.0 summary and compatibility scope in CHANGELOG.md; no release tag created | Finalize publication date and bind notes to the verified release revision | Draft complete; publication pending |
| Earlier SIPI compatibility | OLDAP integration describes one application's migration; no general compatibility guide | Explicitly describe the Lua/config/API differences that have been verified; state that SIPI-2 is not asserted to be a drop-in replacement | Documentation |
| Automation | No tracked .github workflow exists | Decide CI scope and add a reproducible public-fixture build/test workflow; separate expensive S3/container/validator gates and keep release publication explicit | Engineering workflow |
| Publication data review | Earlier credential scan was limited; tracked historical evidence contains workstation paths/private host references | Review intended publishable tree/history and asset scope; approve retention or produce labelled sanitized evidence. Keep original evidence private when sanitizing | Maintainer review |

Missing CI or templates are workflow gaps, not proof that the server fails. Unknown
contact choices cannot be settled by inventing text. No license for an
image is inferred merely from the repository's source-code license file.

## Source and image identity findings

`git tag` and `git ls-remote --tags origin` returned no tags during this review.
The image reference inspected locally was `lrosenth/sipi-2:0.1.3`; its only OCI
label was `org.opencontainers.image.version=24.04`, inherited from the base image.
That is not the executable's version. At the initial review, `Dockerfile` and `scripts/docker_image.py`
did not set SIPI source/revision labels or reject a dirty source tree
before publication. The workflow verifies builds before pushing, but a successful
smoke test does not establish which Git commit a previous image came from.

Do not retroactively tag the current source as the source of an older image based
only on `--version`. Select and verify an intentional release snapshot. The
registry digest inspected earlier remains recorded in the release work record;
this step did not re-pull manifests or make a new source-equivalence claim.
GitHub visibility, enabled reporting features and GitHub Release objects were not
queried: the local GitHub CLI is unavailable. No statement about them is inferred
from the remote URL or absence of Git tags.

## Documentation and preservation findings

The previous 1,532-line master context mixed early TIFF-only milestones with later
full delivery and proposed work that was already complete. It is now replaced by
a concise architecture/workflow/preservation overview. Its archive preserves all
original text with only two relative Markdown links adjusted; the original hash
and archive scope are recorded there. `CODEX_LOG.md` remains the ongoing work log.

The architecture discussion page also mixes historical implementation stages.
Its opening now identifies that scope and directs readers to current contracts.
The dated conformance audit already identifies later results and superseded
findings; do not flatten it into an undated certification claim. A future dedicated
current architecture guide can draw from the concise master context without
silently rewriting design history.

No blanket cleanup is appropriate for ignored build outputs: `build/` contains
private prepared images and original benchmark evidence. The design directories
contain more than the four chosen wordmarks. Keep them until an explicit asset
selection/archival decision; do not treat them as automatically disposable ballast.

## Recommended next sequence

1. Retain the completed macOS ARM64 fresh-checkout evidence below while resolving
   asset attribution and contribution/security channels; verify other intended
   release platforms and separate integration gates against the chosen snapshot.
2. Add contributor, security, release-note and verified migration entry points from
   confirmed choices; review dependency notices and public evidence scope.
3. Strengthen release provenance and choose the release revision/artifact mapping;
   validate the intended platform artifacts and record results.
4. Review the resulting release candidate and publish only as an explicit action.

Public DNS, ACME issuance/renewal, production permissions, browser-cookie behavior
and service-manager stop grace are deployment acceptance checks, distinct from a
public-source release. No production configuration was changed in this review.

## Fresh native verification completed (2026-10-13)

A separate local clone at `54f8e5403a78d533ea4be4b3b9edd612c9711391` was prepared
without shared Git objects, generated builds or private originals. A new Conan home
held the detected profile and exported Grok recipe. The user completed dependency
installation/build and reported 69 passing tests; the retained CTest log confirms
69 passes and no failures. CMake identifies this checkout/toolchain, Debug and
`SIPI_TEST_S3=OFF`; the binary links Grok from the separate Conan home.

The new binary reports 0.1.3. On macOS ARM64 with Apple Clang 21, SDK 27.0,
Conan 2.33.0, CMake 4.4.3 and Ninja 1.13.2, 92 further checks pass against the actual
native TOML, alias policy, JWT policy and documented token recipe. Coverage includes
320×200 info/PNG, 160×100 JPEG, public IDs/redirects, GET/HEAD, CLI precedence,
persistent-cache restart/stable output, valid credentials and denial after cache
fill. Only ports/public URLs and dedicated temporary cache paths were adapted.
All owned servers and temporary secrets/caches were cleaned up; the tested source
checkout remains clean.

The local evidence directory is the sibling `sipi-2-release-check-state/`: retained
CTest output, toolchain/build-origin records, a reproducible example-check script
and JSON results with source revision and binary SHA-256. A full install/build
transcript was not retained; no assertion that every dependency built from source
is made. Host tools/SDK are shared, so this is not a second-machine portability
claim. Docker/MinIO, external IIIF validation and other platforms remain separate.

## Integration follow-up completed (2026-10-13)

At the same clean source revision `54f8e54`, enabling S3 tests and compiling their
additional target produced two passing integration suites (`s3-byte-source`,
`s3-http`) plus four required fixture setup steps. The existing loopback development
MinIO was reused without restarting it; uniquely owned disposable buckets were
cleaned up. This supplements, rather than reruns, the earlier 69 native tests.

The unchanged upstream validator at `f05e365a1cb28f8707cbd50466a86d8f611d1ab8`
passed 78 selected tests: 31 Level 2 and eight optional on each original and
restricted-public service. Exact public-reference pixels and independent WebP
signature/decoding checks passed. The known broken upstream Python-3 `format_webp`
test remains excluded as documented in [external validation](external-validation.md);
this is not certification. The existing validator checkout/venv were reused.

A local Linux ARM64 Docker build from the clean checkout produced test tag
`sipi-release-check:54f8e54-arm64`, image ID
`sha256:81affa674960bb4e2b48741e9444480ab63497daa78b9b8848a4a5b24e303b5b`.
Runtime smoke checks passed for health, OLDAP/JWT, prefixes, info/PNG, cache/denial,
read-only media/Lua mounts, environment settings and JPEG sampling. BuildKit caches
were reused; this was not an uncached Linux dependency bootstrap. The image remains
local, with no registry publication or replacement of running services.

Detailed evidence is retained in sibling `sipi-2-release-check-state/` as
`integration-results.json`, `external-validation.json` and per-stage logs. The
isolated source and upstream validator remain clean; S3 remains enabled only in
the isolated generated build configuration. Test containers/volumes/buckets are
removed. Current AMD64 acceptance, public deployment checks and release artifact
provenance remain open.

## AMD64 functional verification completed (2026-10-13)

Built the same clean `54f8e54` snapshot with `--platform linux/amd64` and loaded
`sipi-release-check:54f8e54-amd64`. Image ID:
`sha256:5e3ca6e34c4f2b9a11b0125854ffd5dc6f961cd2ee75d115d19ad21270ccbd92`.
The Docker engine is `aarch64`; build and execution use AMD64 emulation on this
ARM64 host. Architecture inspection and the complete existing container smoke suite
passed, including runtime contents/user, version/health, OLDAP JWT/prefix handling,
info/PNG, cache/denial, read-only mounts, environment configuration and JPEG sampling.

The local evidence directory retains `amd64-results.json`, builder inspection and
full build/smoke logs. BuildKit/Conan caches were available; this is not a fully cold
Linux bootstrap or native x86 performance measurement. Source checkout remains
clean, test containers/volumes were removed and existing services were unchanged.
Both architecture test images remain local and unpushed. Final release provenance,
publication metadata and production deployment acceptance are still open; if native
x86 execution is required for the support claim, verify it on that hardware separately.

## Release provenance workflow implemented (2026-10-13)

The helper now supplies CMake version, canonical source URL, full Git revision and
clean/dirty state as image labels, checks them with the image architecture, and
records per-platform image IDs/digests plus smoke results under `build/releases/`.
Publication rejects pending staged/unstaged/untracked files, uses a fixed Git archive
for all builds and rechecks Git state between stages. Its final Buildx push records
the actual returned digest; interrupted/failed pushes remain explicitly uncertain.
Raw Docker builds use unknown labels rather than an inherited Ubuntu version.

Ten Python regression cases cover real Git dirty-state detection, fixed-archive
orchestration, labels, blocked publication, metadata validation and failed-push
reporting. A real ARM64 local build with pending edits passed the full container
smoke suite and reported version 0.1.3, base revision 54f8e54 with a dirty suffix,
image identity and no publication attempt. The publish path was mocked only: no
registry push, Git tag or commit was made. This validates the workflow, not a new
release artifact. See [operator instructions](makefile.md#image-identity-and-release-evidence).

The older source/image findings above remain historical evidence. The intended
clean release commit, version/tag policy and final artifact verification are still
required. Mutable external dependencies and the final cached multi-platform build
are not a claim of bit-identical independent rebuilds or signed attestation.
