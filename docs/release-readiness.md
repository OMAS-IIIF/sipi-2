# Public release readiness review

Reviewed 2026-10-13 against the working tree based on `dd8675e` and the pending
operations documentation. This is a release-preparation assessment, not a release
announcement or a declaration that all gates pass. No visibility, source tag,
registry tag or repository setting was changed.

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
| Fresh checkout | Examples used existing builds/caches; no isolated clean native build has been recorded for this preparation | Build from a named source snapshot with isolated generated/Conan state; run normal tests and first-image/config/policy examples; record platform/compiler and failures | Engineering verification |
| Release provenance | CMake and running binary say 0.1.3; local and remote Git tag listings are empty. Inspected local published-image labels expose only inherited Ubuntu version `24.04`, no SIPI source revision | Select a source revision; retain source/tag, build inputs and resulting image digest together; add reliable OCI source/revision/version metadata in the release workflow and verify it | Engineering and release decision |
| Platform acceptance | ARM64 published-image checks are current; AMD64 manifests exist and older runtime evidence is recorded | Verify the intended release artifacts on the supported platforms, marking emulated versus native execution and avoiding emulated performance claims | Engineering verification |
| Image/brand attribution | Owner identified seven originals as their own; chosen public image attribution/license is still unspecified. Brand assets have no separate published attribution statement | Owner selects attribution and applicable image/brand terms; record them beside assets and keep excluded originals/derivatives private | Owner decision |
| Code/dependency notices | Root AGPLv3 text and compact-ICC attribution are present; a complete shipped-component notice inventory is absent | Confirm intended project declaration/attribution, inventory distributed components and preserve their applicable notices in release artifacts | Maintainer review |
| Contributions and support | No CONTRIBUTING.md or issue/PR templates; public docs explain builds but not contribution routing or expectations | Provide contribution/support entry point with scoped changes, test tiers and policy for reports; confirm the actual channel before publishing it | Documentation and owner decision |
| Vulnerability reports | No SECURITY.md or confirmed private reporting channel/support window | Owner selects an available private channel and supported-release policy; document it without inventing an email or promising an SLA | Owner decision |
| Release notes | No CHANGELOG.md or Git release tags found | Add a factual release summary with feature scope, known limits and upgrade/migration boundaries tied to the chosen revision | Documentation |
| Earlier SIPI compatibility | OLDAP integration describes one application's migration; no general compatibility guide | Explicitly describe the Lua/config/API differences that have been verified; state that SIPI-2 is not asserted to be a drop-in replacement | Documentation |
| Automation | No tracked .github workflow exists | Decide CI scope and add a reproducible public-fixture build/test workflow; separate expensive S3/container/validator gates and keep release publication explicit | Engineering workflow |
| Publication data review | Earlier credential scan was limited; tracked historical evidence contains workstation paths/private host references | Review intended publishable tree/history and asset scope; approve retention or produce labelled sanitized evidence. Keep original evidence private when sanitizing | Maintainer review |

Missing CI or templates are workflow gaps, not proof that the server fails. Unknown
license/contact choices cannot be settled by inventing text. No license for an
image is inferred merely from the repository's source-code license file.

## Source and image identity findings

`git tag` and `git ls-remote --tags origin` returned no tags during this review.
The image reference inspected locally was `lrosenth/sipi-2:0.1.3`; its only OCI
label was `org.opencontainers.image.version=24.04`, inherited from the base image.
That is not the executable's version. `Dockerfile` and `scripts/docker_image.py`
currently do not set SIPI source/revision labels or reject a dirty source tree
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

1. Perform the isolated fresh-checkout build/test while asset attribution and
   contribution/security channels are being settled. Use a snapshot that includes
   the intended pending docs/examples; do not silently test only an older HEAD.
2. Add contributor, security, release-note and verified migration entry points from
   confirmed choices; review dependency notices and public evidence scope.
3. Strengthen release provenance and choose the release revision/artifact mapping;
   validate the intended platform artifacts and record results.
4. Review the resulting release candidate and publish only as an explicit action.

Public DNS, ACME issuance/renewal, production permissions, browser-cookie behavior
and service-manager stop grace are deployment acceptance checks, distinct from a
public-source release. No production configuration was changed in this review.
