# Dependency security updates

Run commands from the repository root. Keep exact versions in conanfile.txt and
commit conan.lock alongside it. A plain --update does not upgrade an exact version
or release a locked recipe revision. Prefer one security fix per change.

## Procedure

1. Check the upstream security advisory: affected versions, first fixed release,
   and whether SIPI's build/options are affected. Record the advisory/CVE and chosen
   version in CODEX_LOG.md. Start from a saved Git state so the diff is reviewable.
2. Change the affected version in conanfile.txt. For an indirect dependency, inspect
   the Conan dependency graph and update the dependency that brings it in, or add
   a compatible explicit requirement. Do not edit only the lockfile and assume the
   vulnerable version is gone. Grok uses our own conan/recipes/grok recipe: update
   its pinned sources/checksums/version as needed, then conan export that directory.
3. Resolve and install, deliberately replacing the previous lock:

```sh
conan install . \
  -pr:h profiles/macos-debug \
  -pr:b sipi-2-local \
  --update \
  --lockfile="" \
  --lockfile-out=conan.lock \
  --build=missing

git diff -- conanfile.txt conan.lock
```

This unlocks the whole graph: fixed direct version numbers remain fixed, but
recipe revisions and indirect versions may change. Review every lockfile change,
confirm the resolved affected library is fixed, and investigate conflicts rather
than forcing incompatible dependencies. Recipe revisions can change build patches
without changing the upstream version number.

For a new direct version that needs no other locked dependencies changed, the
narrower alternative is to keep conan.lock and replace --lockfile="" above with
--lockfile=conan.lock --lockfile-partial --lockfile-clean. Existing locked entries
remain pinned; this does NOT refresh a same-version locked recipe or automatically
fix a vulnerable indirect dependency. Check the resulting graph either way.

4. Reconfigure, rebuild and test; reload CMake in CLion:

```sh
cmake --preset conan-debug
cmake --build --preset conan-debug
ctest --preset conan-debug --output-on-failure
```

Start the documented MinIO test environment for S3 checks. Review applicable
sanitizer and codec/color regressions. If rendering or encoded bytes can change,
review current_render_recipe in src/cache/CacheKey.h and the WebP-specific recipe
in src/cache/CacheKey.cpp so old cached output cannot masquerade as the new result.
5. Commit conanfile.txt, conan.lock, any recipe/code changes and CODEX_LOG.md together.
   Rebuild the actual deployment configuration/container too, ship the new binary
   and required shared libraries, and restart deployed instances. Updating only
   the development Conan cache does not patch a running server.

Reference: [Conan install and lockfile options](https://docs.conan.io/2/reference/commands/install.html).

## JPEG provider switch (2026-10-08)

`libjpeg-turbo/3.2.0` replaces `libjpeg/9f`; `libtiff/*:jpeg=libjpeg-turbo`
selects the same provider for TIFF. SIMD is enabled and the TurboJPEG wrapper API
is disabled. The lock change preserves unrelated revisions and Linux-only entries,
adding NASM for Intel builds. A clean lock resolved only on macOS can drop Linux
conditional dependencies; keep or separately resolve the deployment graph too.

Rebuild all consumers because the old v9 and new v6b-compatible ABIs differ.
Clear stale `libjpeg-turbo_DIR` CMake cache entries before configuring an existing
build if they point outside Conan. The migration verified one Conan JPEG archive
and matching 3.2.0 headers in both local configurations. See
[JPEG backend notes](jpeg-delivery.md#jpeg-backend-and-rebuilds).

## License review

When changing a dependency, review its upstream license and bundled components.
Refresh `THIRD_PARTY_NOTICES.md` and the exact-reference, checksum-verified
supplements under `third_party/notices/`; do not bypass collector failures by
changing reference strings alone. Docker captures `conan install --format=json`
and uses `scripts/collect_licenses.py` for every host package, including static
and header-only dependencies. A recipe's license label is not a substitute for
its full terms or notices. Preserve the required sources, patches and recipes
for publication with the release; the notice inventory is not a source bundle.
