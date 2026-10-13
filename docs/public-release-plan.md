# Public release preparation

Status: inventory, third-party image history cleanup, README/navigation refresh,
feature catalog, Docker/native configuration, Lua-preflight and operations guides
completed on 2026-10-13; remaining documentation and release checks are listed below.
Baseline: commit `2d26200`, CMake version `0.1.3`. This is a preparation plan,
not a release announcement. Later sections record registry/runtime verification.

## Current follow-up

The [readiness review](release-readiness.md) is the current remaining-gate list.
The inventory and gap map below preserve the starting assessment; completed items
are recorded in dated sections and are not reopened by historical wording.
Master context consolidation is complete, with the old text archived intact apart
from relative link adjustments. Fresh macOS ARM64 checkout validation is now complete (69 native tests and 92
example checks); see the readiness review for scope. Remaining work concerns
deployment acceptance, release provenance, asset attribution, contribution/security
channels and notices. S3/external-validator and both local Docker architecture
checks now pass; AMD64 execution is emulated, not native x86.

## Inventory and immediate decisions

The starting checkout contained 365 tracked files, including 67 Markdown files.
Local sizes below are rounded filesystem usage; they are not compressed Git or
container sizes. Existing logo work and its log entries were uncommitted before
this audit and must be preserved.

| Area | Observed state | Action |
| --- | --- | --- |
| `src/`, `tests/`, CMake, Conan, scripts | Active implementation, regression coverage and build/release workflow | Keep; verify fresh-checkout workflows later |
| `test-images/` | About 235 MiB; nine tracked original image files plus Finder metadata at baseline | Preserve originals; resolve provenance and redistribution before public repository publication |
| `build/` | About 5.1 GiB, ignored; mixes compiled outputs, original benchmark results, prepared image corpus and external validator environment | Do not delete wholesale; classify subdirectories and preserve non-reproducible evidence first |
| `cmake-build-debug/` | About 704 KiB, ignored local build | Candidate for later removal after checking IDE usage; no public repository benefit |
| `.idea/`, `CMakeUserPresets.json` | Ignored local IDE/generated settings | Keep local; do not publish |
| `uploads/` | Empty, ignored runtime directory | No release work needed |
| `benchmarks/` | About 500 KiB local; includes reproducible comparison tooling | Keep; do not mistake experiments for disposable files |
| `benchmarks/reference/` | Frozen resampler used by the opt-in Docker comparison target | Keep: Dockerfile directly depends on it |
| `third_party/compact-icc/` | Profile files, source attribution and CC0 license | Keep: regression tests read these files and production embeds their bytes |
| `docs/` | About 5.7 MiB, mostly detailed contracts and audit/benchmark evidence | Reorganize navigation and separate current guidance from dated evidence |
| `codex.md` | About 100 KiB; has accumulated a development chronology | Consolidate into a concise current architecture/workflow overview, preserving useful history |
| `CODEX_LOG.md` | About 156 KiB technical history | Retain as descending work log; keep distinct from public onboarding |
| `logo/` | Four user-selected SVG/PNG files, currently untracked | Canonical selected wordmarks; include deliberately in public assets |
| `assets/branding/` | About 564 KiB; drafts, previews and standalone icon variants | Preserve until selected icons/export notes are moved or retained explicitly; not all files are duplicates |
| `.DS_Store` | Root ignored, nested tracked metadata found | Removed root and `test-images/.DS_Store`; ignore at every depth |

All four selected `logo/` files match their counterparts under
`assets/branding/flowing-s/` byte for byte. The latter directory also contains
standalone icons, monochrome variants and notes that are not in `logo/`.

The large `build/` entries include `archive-images` (~780 MiB),
`archive-compare-2026-10-08` (~674 MiB), `sanitize` (~551 MiB), `resize-docker`
(~528 MiB), `http-tsan-instrumented` (~502 MiB), and the Debug build (~471 MiB).
These have different preservation needs. An ignored directory does not increase
the files shipped by Git; deleting it only recovers local disk space.

## Publication issues to resolve

### Original image provenance

The owner clarified provenance on 2026-10-13: the six `pharch_*` TIFF/JP2
files and `sample-tif-01.tif` are their own images. `sample-tiff-02.tif` is
third-party with source still unknown; the Swiss map is from swisstopo. See the
[corpus attribution table](archive-test-corpus.md#source-attribution-and-publication-review-2026-10-13).

The owner subsequently requested retaining those two third-party originals locally
for tests, removing them from GitHub where possible, and adding ignore entries.
Commit `73254b7` untracks both files and adds exact root-anchored ignore rules;
it has been pushed to `origin/main`. Both local SHA-256 hashes are unchanged.
Prepared derivatives remain private too. This does not assert that swisstopo
prohibits reuse. Record attribution/license for the owner's other seven files.

The owner approved the history cleanup, and the guarded push completed on
2026-10-13. The cleaned `main` tip on GitHub and locally was `d74258c` immediately after
that rewrite; subsequent ordinary commits advance it.
Eight of 18 commits changed; every other file in every corresponding commit
matches exactly. Both excluded paths and blob IDs are absent from the reachable
`main` history. A private Git bundle outside the repository preserves the original
history. Backup/original refs were not pushed.

The local branch was reconciled by changing its reference only after verifying
that the old and new tip trees were identical. All 400 inventoried working files,
including ignored originals and pending branding/documentation, matched their
pre-push hashes; the pending diff and status also matched. No reset or checkout
was needed. Existing clones and GitHub-retained objects may still contain old
versions; do not claim deletion of every copy. Avoid merging or force-pushing the
old history back into the public branch.

Keep a reproducible synthetic-image path for ordinary public build/test
instructions. Historical corpus measurements still refer to the original private
inputs and must not be silently relabelled as measurements of replacement files.

### Local information and credentials

A limited scan covered current tracked text and all locally reachable history:
863 reachable Git objects, 695 blobs no larger than 2 MB, of which 692 were treated
as text. It found no private-key headers or selected AWS/GitHub credential token
signatures. Historical filename checks found no conventional `.env`, private-key
or credential file names. This does **not** establish absence of arbitrary
passwords, JWTs, unrecognized tokens, secrets in large/binary files, unreachable
objects, remote-only refs, or untracked local files.

Current password/key assignment matches include explicit test/example fixtures;
do not delete those merely because a keyword scanner flags them. Development
MinIO credentials are explicitly public loopback-only test credentials.

Absolute workstation paths occur in tracked audit and benchmark JSON, including
`docs/audits/2026-10-08-external-validator.json` and
`docs/benchmarks/2026-10-12-htj2k.json`. Private host references occur in the x86
benchmark reports, `docs/benchmarks/resize-docker.md`, and the technical log.
Prepare portable public commands; retain historical raw evidence or create clearly
labelled sanitized derivatives rather than silently changing recorded provenance.
A complete publication review must include the history chosen for publication.

### Public project metadata and release artifacts

- `LICENSE` contains the GNU AGPL version 3 text. Confirm project attribution and
  intended license declaration without inventing an author, year, or licensing
  exception. Inventory notices required for distributed dependencies/components.
- No `.github/` directory, contributor guide, changelog or security contact guide
  was present at inventory time. Define contribution/support/security channels;
  add CI around reproducible public workflows when credentials and hosting exist.
- `make docker-build` publishes after verification; it is not a local-only build.
  Onboarding must lead with pulling a verified tag or `make docker-local`.
- CMake currently says `0.1.3`. Registry inspection on 2026-10-13 confirmed
  `lrosenth/sipi-2:0.1.3`, index digest
  `sha256:84f810e8b886e0a6a3abbd24ae1a63c254cd4e4715140c9bdbee333e1ac9d806`, with
  AMD64/ARM64 runtime manifests. Pull and native ARM64 quickstart passed, reporting
  version 0.1.3. Exact source/tag provenance still needs release verification;
  a matching version string alone does not prove source identity.
- Docker's allowlist excludes original images, logs and documentation. The runtime
  uses UID/GID 10001, port 8080, `/data/images`, and `/var/cache/sipi`; these are
  concrete starting points for the operator guide, not evidence of a registry push.

## Documentation structure and gap map

Use English throughout repository documentation. Keep each reference authoritative
and link to it rather than duplicating defaults or API contracts across guides.

| Reader need | Existing source | Required change |
| --- | --- | --- |
| Understand the product and get a first image | `README.md`, `logo/` | Replace chronological opening and hello-route walkthrough with product overview, release status, highlights and verified quickstart |
| See all supported features and limitations | Format guides, `iiif-info.md`, `iiif-http.md`, `output-limits.md`, color/alpha guides | Add `docs/features.md` with input/output matrix, IIIF capabilities, sources, policy, cache and explicit exclusions |
| Use a published container | `docs/docker.md`, `docker/compose.example.yaml` | Make runtime instructions primary; cover pinned tag, ports, mounts, ownership, persistence, config and generic examples before OLDAP/build details |
| Compile and run natively | README, `docs/makefile.md`, Conan profiles, Docker build recipe | Add `docs/building.md`; document fresh checkout prerequisites, platform evidence, Debug/Release and first image without assuming local compiler/cache state |
| Configure consistently | `docs/command-line.md`, `sipi.example.toml`, `src/CommandLine.cpp`, config tests | Consolidate option names/types/defaults/precedence and restart behavior; Docker links here for shared settings |
| Write Lua policies | `docs/preflight.md`, `lua-*.md`, `examples/` | Lead with operator-facing lifecycle, input/output contract and incremental runnable examples; retain C++ details in a later section |
| Operate and diagnose | HTTP/cache/worker/config guides | Add `docs/operations.md` covering proxy/TLS, base URL, CORS, logs, health, limits, storage, upgrades and common failures |
| Understand architecture and contribute | `docs/iiif-delivery-architecture.md`, tests, dependency guide | Add contributor entry point and test tiers; keep project-specific policies outside generic quickstart |
| Compare with earlier SIPI | `docs/oldap-preflight.md`, current contracts | Document verified compatibility/migration boundaries; do not imply drop-in equivalence |
| Find historical validation | `docs/iiif-conformance.md`, audits and benchmark reports | Provide a clear current status and label dated findings; preserve evidence and qualified performance claims |

Add `docs/README.md` as the human-readable navigation page. Keep the root README
short enough to scan; no documentation-site framework is needed for this step.
Use existing topic names when practical so links remain stable.

Specific stale or confusing content found at inventory time (README and build
onboarding items below were subsequently addressed by the refresh):

- README's opening describes early syntax routes and an incomplete delivery subset.
- Its direct-dependency summary omits WebP and LittleCMS present in `conanfile.txt`.
- Native instructions assume `sipi-2-local` and a macOS Debug profile. The Makefile
  configure path defaults to Debug; a Release guide must verify the actual commands.
- `docs/preflight.md` begins with C++ construction before explaining the Lua API.
- `docs/docker.md` starts with building/publishing and is titled around an OLDAP trial.
- `docs/iiif-conformance.md` retains superseded warnings below later successful
  checks. Historical scope must be unmistakable to avoid contradictory guidance.
- `third_party/compact-icc/README.md` has an explicitly dated statement that
  LittleCMS was not a dependency for that change. Preserve the historical context
  and avoid presenting it as today's dependency inventory.

A file-target check of relative inline links in all 67 tracked Markdown files found
no missing targets. It did not validate anchors, reference-style links, remote URLs
or whether the target's contents are current.


## README/navigation refresh completed (2026-10-13)

- Replaced the chronological root README with product scope, features/limits,
  selected light/dark logo, pinned Docker quickstart and configuration/Lua entry points.
- Added `docs/README.md` topic navigation and `docs/building.md` for native build,
  Debug/Release, tests and IDE guidance; updated the Makefile guide's build link.
- Added an original procedural RGB8 PNG and reproducible standard-library recipe
  under `examples/images/`; no private images are required for onboarding.
- Verified 100 local links/assets, including referenced Markdown anchors in the
  five edited/new guides. The documented image recipe reproduces the supplied PNG.
- Tested the published 0.1.3 image on native ARM64 and the existing native Debug
  binary: health/version, 320 × 200 info with correct public ID, 160 × 100 JPEG
  preview, and full-size PNG. Test-owned container/process were stopped afterward.
- Scope limits: existing native binary, not a clean rebuild; AMD64 manifest checked,
  not runtime-tested in this step. The feature catalog was completed in the follow-up
  below; Docker/Lua/operations restructuring and clean-checkout verification remain open. No image/release push.

## Feature catalog completed (2026-10-13)

`docs/features.md` catalogs input/output subsets, IIIF syntax/discovery, color and
transparency, local/S3 storage, policy/resolution/watermarks, Lua helpers, caches,
concurrency, configurable versus fixed bounds, HTTP/deployment and exclusions.
README and the documentation index link to it. Six existing format/information
pages were corrected where old text contradicted implemented capabilities.

Source/default/contract review was supplemented by 21 focused HTTP probes against
the existing native 0.1.3 Debug binary using the synthetic example: discovery,
output limits, region/size forms, mirror/rotation, quality variants, all three
encoders, rejected TIFF output, and disabled/enabled enlargement. No fresh build,
full suite, S3/Docker retest or broad format conformance claim is implied. Local
links and explicit anchors were checked; no runtime implementation changed.

## Docker operator guide completed (2026-10-13)

Rewrote `docs/docker.md` around published-image use rather than image building or
an OLDAP trial. Added mount/permission and Docker-setting tables, three executable
paths, secret/S3 networking guidance, proxy-prefix rules, cache persistence,
update/rollback and troubleshooting. The new `examples/docker/sipi.toml` and
`preflight.lua` use the public synthetic sample only. The existing Compose service
configuration remains unchanged apart from explanatory comments. Historical OLDAP
trial specifics were preserved in `docs/oldap-preflight.md`.

Verified published image 0.1.3 on native ARM64: minimal/configured/Compose delivery,
health/version/info/JPEG/PNG, explicit 403 denial, UID/GID 10001, read-only root and
writable cache, two persisted encoded objects, identical PNG after restart,
Compose volume retention on down, CLI-over-environment and environment-over-TOML,
and missing bind failure without directory creation. The image has no HEALTHCHECK,
Python or curl; probes run on the host. All test-owned containers/volumes were
removed. Registry AMD64/ARM64 manifests were rechecked; AMD64 execution, full S3,
proxy/TLS and actual upgrade/rollback were not rerun or enacted. No image rebuild,
push or deployment modification.

## Native configuration guide completed (2026-10-13)

Reworked `docs/command-line.md` into a native quickstart and shared option reference:
36 configurable keys with environment bindings, defaults/types/limits, three
command-only options, precedence, working-directory paths, permissions, restart
requirements and error behavior. Preserved detailed topic anchors. Added the
runnable `examples/native/sipi.toml` and completed JPEG entries in the annotated
root template; updated onboarding links.

Checked table and template coverage against parser registrations/environment
bindings. All three focused CTest suites passed (startup, S3 and script
configuration). Sixteen isolated checks with the existing native Debug 0.1.3
binary passed: sample/info/JPEG/cache, precedence, inherited/explicit height,
working-directory behavior, help and invalid-input cases, and root-template
startup. Test-owned processes and temporary directories were cleaned up. No new
build, Docker/S3 network test, production code change, commit or push in this step.

## Lua-preflight operator guide completed (2026-10-13)

Reorganized `docs/preflight.md` around runnable native examples, request/decision
field tables, source mapping, authorization before cache lookup, lifetime/restart,
errors, helpers and detailed contracts. Moved C++ usage behind operator guidance.
The minimal policy now maps only `/example` to the public synthetic sample; added
`examples/preflight-jwt.lua` with explicit issuer/audience, identifier/read and
prefix checks. Its documented Python recipe generates an ephemeral local demo key
and expiring token. Updated navigation and stale context/diagnostic statements.

Four existing suites passed: lua-preflight, preflight-context-http,
script-config-http and lua-jwt. Forty-four isolated native HTTP checks exercised
both actual example files and the exact token-generation recipe: GET/HEAD for
info/image/base, allowed/denied aliases, cached authorization, bad/expired tokens,
claim binding, no query fallback, script snapshots/restart and contract-error
mapping. Temporary processes/files were cleaned up. Existing native Debug 0.1.3
binary used; no rebuild, Docker/S3/OLDAP deployment or production-code change.

## Operations guide completed (2026-10-13)

Added `docs/operations.md` for deployment boundaries, proxy/TLS/public identity,
liveness versus delivery monitoring, logging, diagnosis, concurrency/timeouts,
shutdown, cache maintenance/recovery and updates/rollback. Added a configurable
Caddyfile with explicit routing order, `/iiif` stripping and private health/demo
routes; linked the guide from README/index/Docker. Corrected obsolete original-only
variant and codec claims in the persistent-cache reference.

Caddy 2.8.4 configuration validation and 24 isolated proxy HTTP checks passed with
published SIPI 0.1.3 on native ARM64, including external identities, encoded prefix,
GET/HEAD, CORS/OPTIONS, blocked routes and per-request JWT authorization after cache
fill. Four selected existing native suites plus two required fixture setups passed.
Only test-owned containers/networks were created and removed. No public DNS/ACME,
production service-manager deployment, S3 backend, fresh build or image publication
was performed; these are distinct acceptance checks.

## Context consolidation and readiness review completed (2026-10-13)

Reduced root `codex.md` from 1,532 to 155 lines, covering current architecture,
invariants, key files, build/style conventions, preservation and next release gates.
Archived the former text under `docs/history/`, adjusting only two relative links;
reconstruction matches its recorded SHA-256. Marked the architecture discussion
page as historical and linked the current overview.

Added `docs/release-readiness.md` as the current actionable gap list. Read-only
checks confirmed absent local/remote Git tags, missing contribution/security/release
metadata and CI, seven tracked owner originals, ignored excluded paths with no
locally reachable path history, and missing SIPI source/version labels in the
locally inspected published image. Exact old image/source identity is unresolved;
GitHub visibility/settings/releases were not inspected because gh is unavailable.
All 520 local inline Markdown links/anchors resolve. No runtime changes or tests,
asset deletion, history rewrite, tagging, publication or repository setting changes.

## Release provenance workflow implemented (2026-10-13)

OCI source/version/revision/state labels and local JSON evidence now link helper
builds to their Git/CMake inputs. Publish rejects dirty trees, uses a fixed Git
archive and checks source state between verification/build stages. Reports retain
platform image IDs/digests, smoke results and eventual push metadata/uncertainty.
Ten mocked/real-Git regression cases and a real local ARM64 development-image smoke
pass. No commit, Git tag or push was made. The clean release snapshot/tag policy,
final published artifact acceptance and old-image provenance remain separate work.

## Execution order and completion criteria

1. **Inventory — completed.** Record evidence, preservation rules, publication
   questions and documentation destinations. Remove only identified Finder metadata.
2. **Repository cleanup.** Resolve corpus publication strategy; preserve raw
   evidence; consolidate selected branding and project context; update ignores
   and broken references only as needed. No blanket `git clean` or `build/` removal.
3. **Public documentation.** Build navigation, feature catalog, native/container
   guides, configuration/preflight guidance, operations and contribution metadata.
   Check each claim against current implementation/tests, not just old prose.
4. **Fresh-checkout verification.** In an isolated temporary checkout, run the
   documented native build and first-image scenario; pull the selected public
   image and exercise generic container examples without local Conan/build state.
   Verify TOML/CLI/environment precedence, mounts, cache permissions and a minimal
   Lua policy. Separate actually tested platforms from merely expected portability.
5. **Release review.** Confirm source tag/version/image mapping, public history,
   attribution and asset provenance, links, changelog and support channels. Publish
   only as a deliberate release action; this plan does not publish anything.

Record commands, outcomes and limitations for the verification step. Documentation
and ignore-file changes alone do not warrant rerunning the expensive server suite;
run appropriate checks when examples, configuration or implementation change.
