#!/bin/sh
# Run the six-pair archive comparison from the extracted, read-only corpus.
# JSON goes to stdout; progress/hash checks go to stderr. Requires Docker/coreutils.
set -eu
cd "$(dirname "$0")"
corpus_dir=$(pwd -P)
sha256sum -c SHA256SUMS >&2
set --
for image in pharch_01 pharch_02 pharch_03 sample-tif-01 sample-tiff-02 swiss-map-raster500_500_komb_25_2056; do
    set -- "$@" "/images/$image/pyramid.tif" "/images/$image/image.jp2"
done
exec docker run --rm --platform linux/amd64 --network none --read-only \
    --cap-drop ALL --security-opt no-new-privileges --memory 2g \
    --mount "type=bind,src=$corpus_dir/images,dst=/images,readonly" \
    sipi-benchmark/resize-axis:2026-10-12-amd64 "$@"
