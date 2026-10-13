# syntax=docker/dockerfile:1
FROM ubuntu:24.04 AS build
ARG DEBIAN_FRONTEND=noninteractive
ARG BUILD_JOBS=4
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential python3 python3-venv git cmake ninja-build pkg-config \
    ca-certificates autoconf automake libtool nasm && rm -rf /var/lib/apt/lists/*
RUN python3 -m venv /opt/conan && /opt/conan/bin/pip install --no-cache-dir conan==2.33.0
ENV PATH="/opt/conan/bin:${PATH}"
WORKDIR /src
COPY conanfile.txt conan.lock ./
COPY conan/ conan/
COPY docker/conan-profile docker/conan-profile
RUN --mount=type=cache,target=/root/.conan2,sharing=locked \
    conan profile detect --force && conan export conan/recipes/grok && \
    conan lock create . -pr:h docker/conan-profile -pr:b docker/conan-profile \
      --lockfile=conan.lock --lockfile-partial --lockfile-out=/tmp/docker.lock && \
    conan install . -pr:h docker/conan-profile -pr:b docker/conan-profile --lockfile=/tmp/docker.lock --build=missing -s build_type=Release \
      -s compiler.cppstd=20 -c tools.build:jobs=${BUILD_JOBS} \
      --format=json > /tmp/conan-installed.json
COPY CMakeLists.txt ./
COPY src/ src/
COPY benchmarks/ benchmarks/
RUN --mount=type=cache,target=/root/.conan2,sharing=locked \
    cmake -S . -B build/Release -DCMAKE_TOOLCHAIN_FILE=build/Release/generators/conan_toolchain.cmake \
      -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF && \
    cmake --build build/Release --parallel ${BUILD_JOBS} && \
    mkdir -p /out/lib && cp build/Release/sipi-2 /out/ && \
    ldd /out/sipi-2 | awk '/=> \/root\/\.conan2/ {print $3}' | xargs -r -I '{}' cp -L '{}' /out/lib/ && \
    find /out -type f -exec strip --strip-unneeded '{}' +

# Preserve notices for static/header-only dependencies as well as shared libraries.
COPY scripts/collect_licenses.py scripts/collect_licenses.py
COPY LICENSE LICENSING.md THIRD_PARTY_NOTICES.md ./
COPY LICENSES/ LICENSES/
COPY third_party/ third_party/
RUN --mount=type=cache,target=/root/.conan2,sharing=locked \
    python3 scripts/collect_licenses.py /tmp/conan-installed.json /out/licenses

# Optional source-only export. Restore archives with Conan 2.33.0; no binaries.
FROM build AS source-export-build
COPY scripts/export_sources.py scripts/export_sources.py
RUN --mount=type=cache,target=/root/.conan2,sharing=locked \
    conan install . -pr:h docker/conan-profile -pr:b docker/conan-profile \
      --lockfile=/tmp/docker.lock --build=never -s build_type=Release \
      -s compiler.cppstd=20 -c tools.build:jobs=${BUILD_JOBS} \
      -c tools.build:download_source=True --format=json > /tmp/conan-sources.json && \
    python3 scripts/export_sources.py /tmp/conan-sources.json /source-export
FROM scratch AS source-export
COPY --from=source-export-build /source-export/ /

# Opt-in offline comparison. The normal runtime target below stays the default.
FROM build AS resize-benchmark-build
RUN --mount=type=cache,target=/root/.conan2,sharing=locked \
    cmake --build build/Release --target resize-benchmark --parallel ${BUILD_JOBS} && \
    mkdir -p /benchmark/lib && cp build/Release/resize-benchmark /benchmark/after && \
    sha256sum src/image/AreaResize.cpp benchmarks/reference/AreaResize-before-axis.cpp \
      benchmarks/ResizeBenchmark.cpp conan.lock > /benchmark/source-sha256.txt && \
    cp benchmarks/reference/AreaResize-before-axis.cpp src/image/AreaResize.cpp && \
    touch src/image/AreaResize.cpp && \
    cmake --build build/Release --target resize-benchmark --parallel ${BUILD_JOBS} && \
    cp build/Release/resize-benchmark /benchmark/before && \
    ldd /benchmark/after | awk '/=> \/root\/\.conan2/ {print $3}' | xargs -r -I '{}' cp -L '{}' /benchmark/lib/ && \
    find /benchmark -type f ! -name '*.txt' -exec strip --strip-unneeded '{}' +

FROM ubuntu:24.04 AS resize-benchmark
RUN apt-get update && apt-get install -y --no-install-recommends python3 libstdc++6 libgomp1 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=resize-benchmark-build /benchmark/ /opt/benchmark/
COPY benchmarks/compare_resize.py /opt/benchmark/compare_resize.py
ENV LD_LIBRARY_PATH=/opt/benchmark/lib PYTHONDONTWRITEBYTECODE=1
USER 10001:10001
ENTRYPOINT ["python3", "/opt/benchmark/compare_resize.py"]

FROM ubuntu:24.04 AS runtime
# Raw Docker builds have unknown provenance; the release helper overrides these
# labels from CMake/Git and verifies them after building each architecture.
LABEL org.opencontainers.image.title="SIPI-2" \
      org.opencontainers.image.source="https://github.com/OMAS-IIIF/sipi-2" \
      org.opencontainers.image.licenses="AGPL-3.0-only" \
      org.opencontainers.image.version="unknown" \
      org.opencontainers.image.revision="unknown" \
      org.omas-iiif.sipi.source-state="unknown"
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates libstdc++6 libgomp1 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --uid 10001 --create-home sipi \
    && mkdir -p /var/cache/sipi /data/images && chown sipi:sipi /var/cache/sipi
COPY --from=build /out/ /opt/sipi/
COPY examples/oldap/ /opt/sipi/examples/oldap/
# Ubuntu's per-package copyright files remain in /usr/share/doc.
RUN dpkg-query -W -f='${binary:Package}\t${Version}\t${source:Package}\t${source:Version}\n' \
    > /opt/sipi/licenses/ubuntu-packages.tsv
ENV LD_LIBRARY_PATH=/opt/sipi/lib SIPI_LISTEN_ADDRESS=0.0.0.0
USER sipi
WORKDIR /home/sipi
EXPOSE 8080
ENTRYPOINT ["/opt/sipi/sipi-2"]
