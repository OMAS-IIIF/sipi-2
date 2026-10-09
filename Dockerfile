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
      -s compiler.cppstd=20 -c tools.build:jobs=${BUILD_JOBS}
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

FROM ubuntu:24.04 AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates libstdc++6 libgomp1 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --uid 10001 --create-home sipi \
    && mkdir -p /var/cache/sipi /data/images && chown sipi:sipi /var/cache/sipi
COPY --from=build /out/ /opt/sipi/
COPY examples/oldap/ /opt/sipi/examples/oldap/
ENV LD_LIBRARY_PATH=/opt/sipi/lib SIPI_LISTEN_ADDRESS=0.0.0.0
USER sipi
WORKDIR /home/sipi
EXPOSE 8080
ENTRYPOINT ["/opt/sipi/sipi-2"]
