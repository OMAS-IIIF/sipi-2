# Repository instructions

- Follow the architecture and ownership invariants in [codex.md](codex.md).
- Use the Conan/CMake workflow in [docs/building.md](docs/building.md); run focused CTests for changed contracts and HTTP integration tests for request-flow changes.
- Never delete `build/` wholesale: it also contains private images and retained benchmark evidence.
- Keep private corpus originals and derivatives ignored; follow [corpus provenance](docs/archive-test-corpus.md).
- `make docker-build` publishes images. Use `make docker-local` for local container verification.
