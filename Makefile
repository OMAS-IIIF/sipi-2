# Development/release shortcuts. CMake remains the single version source.
.DEFAULT_GOAL := help
PYTHON ?= python3
BUILD_DIR ?= build/Debug
HOST_PROFILE ?= profiles/macos-debug
BUILD_PROFILE ?= sipi-2-local
JOBS ?= 4
DOCKER_IMAGE ?= lrosenth/sipi-2
PLATFORMS ?= linux/amd64,linux/arm64
VALIDATOR_DIR ?= build/external-image-validator
VALIDATOR_PYTHON ?= build/iiif-validator-venv/bin/python
# scripts/version.py reads VERSION directly from CMake for bumps and image tags.

.PHONY: help show-version bump-patch-level bump-minor-level bump-major-level \
        dependencies configure build minio test test-core test-external \
        test-prerequisites docker-local docker-test docker-build

help:
	@printf '%s\n' \
	  'make show-version       Show the CMake version' \
	  'make bump-patch-level   Increment patch; no commit/tag/push' \
	  'make bump-minor-level   Increment minor and reset patch' \
	  'make bump-major-level   Increment major and reset minor/patch' \
	  'make dependencies       Install locked Conan dependencies (local profiles required)' \
	  'make build              Configure and build native code/tests' \
	  'make test               All CTests including MinIO, external IIIF, native Docker smoke' \
	  'make test-core          CTests including MinIO (starts the development MinIO)' \
	  'make test-external      Unmodified external IIIF validator; no automatic install' \
	  'make docker-local       Build/test a native runtime image, without publishing' \
	  'make docker-test        Build/test amd64 and arm64, without publishing' \
	  'make docker-build       Build/test both architectures, push VERSION and latest' \
	  'Overrides: DOCKER_IMAGE, PLATFORMS, JOBS, BUILD_DIR, HOST_PROFILE, BUILD_PROFILE'

show-version:
	@$(PYTHON) scripts/version.py show

bump-patch-level bump-minor-level bump-major-level:
	$(PYTHON) scripts/version.py $(patsubst bump-%-level,%,$@)

dependencies:
	conan export conan/recipes/grok
	conan install . -pr:h "$(HOST_PROFILE)" -pr:b "$(BUILD_PROFILE)" --build=missing

configure:
	@if test -f "$(BUILD_DIR)/CMakeCache.txt"; then \
	  cmake -S . -B "$(BUILD_DIR)" -DBUILD_TESTING=ON -DSIPI_TEST_S3=ON; \
	else \
	  test -f "$(BUILD_DIR)/generators/conan_toolchain.cmake" || { echo 'Missing Conan toolchain: run make dependencies first.' >&2; exit 1; }; \
	  cmake -S . -B "$(BUILD_DIR)" -DCMAKE_BUILD_TYPE=Debug \
	    -DCMAKE_TOOLCHAIN_FILE="$(abspath $(BUILD_DIR))/generators/conan_toolchain.cmake" \
	    -DBUILD_TESTING=ON -DSIPI_TEST_S3=ON; \
	fi

build: configure
	cmake --build "$(BUILD_DIR)" --parallel "$(JOBS)"

minio:
	docker compose -f dev/minio/compose.yaml up -d --wait

# Check prerequisites before spending time building. Never report skipped suites as passes.
test-prerequisites:
	@docker info >/dev/null
	@test -f "$(VALIDATOR_DIR)/iiif-validate.py" && test -x "$(VALIDATOR_PYTHON)" || \
	  { echo 'External validator missing: follow docs/external-validation.md.' >&2; exit 1; }

# Recursive calls deliberately sequence suites even when the caller uses make -j.
test: test-prerequisites
	$(MAKE) test-core
	$(MAKE) test-external
	$(MAKE) docker-local
	@echo 'All CTests, external IIIF validation and native container checks passed.'

test-core: build minio
	ctest --test-dir "$(BUILD_DIR)" --output-on-failure

test-external: build
	$(PYTHON) tests/external_iiif_validator.py "$(BUILD_DIR)/sipi-2" \
	  "$(VALIDATOR_DIR)" "$(VALIDATOR_PYTHON)" build/external-validation.json

# Publish only after both platform images have passed real container checks.
docker-local docker-test docker-build:
	$(PYTHON) scripts/docker_image.py $(if $(filter docker-local,$@),local,$(if $(filter docker-test,$@),test,publish)) \
	  --image "$(DOCKER_IMAGE)" --platforms "$(PLATFORMS)" --jobs "$(JOBS)"
