# Benchmark-only derivative of the existing local Cantaloupe 5.0.7 image.
# No application configuration, credentials or registry publication is performed.
ARG CANTALOUPE_BASE=lrosenth/oldap-imageserver:latest
FROM ${CANTALOUPE_BASE}
USER root
ARG OPENJPEG_VERSION=2.5.0-2ubuntu0.5
RUN apt-get update -qq \
 && apt-get install -y --no-install-recommends libopenjp2-tools=${OPENJPEG_VERSION} \
 && dpkg-query -W libopenjp2-tools libopenjp2-7 openjdk-21-jre-headless > /opt/iiif/benchmark-packages.txt \
 && rm -rf /var/lib/apt/lists/*
ENV OPJ_NUM_THREADS=1
