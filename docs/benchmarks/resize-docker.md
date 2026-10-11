# Offline Intel/AMD scaling comparison

This opt-in image contains the current resampler and a frozen pre-optimization
reference, built with the same Release flags, dependencies and benchmark driver.
It runs no HTTP service, contacts no API and changes no repository files or caches.
The normal server image and default Docker build target remain unchanged.

## Build and transfer from the development Mac

From the SIPI-2 repository root:

```sh
docker buildx build --platform linux/amd64 --target resize-benchmark \
  --build-arg BUILD_JOBS=6 --load \
  -t sipi-benchmark/resize-axis:2026-10-12-amd64 .
mkdir -p build/resize-docker
set -o pipefail
docker save sipi-benchmark/resize-axis:2026-10-12-amd64 | \
  gzip > build/resize-docker/resize-axis-amd64.tar.gz
scp build/resize-docker/resize-axis-amd64.tar.gz rosenth@media.home.org:~/
```

The build can run under AMD64 emulation on the Mac. **Do not use timings from that
emulation as Intel/AMD performance results.** Actual measurements run natively on
the Intel/AMD VM. No registry publication or server deployment is involved.

## Run on media.home.org

Only Docker is needed on the VM; nothing is installed on its host:

```sh
sudo docker load -i ~/resize-axis-amd64.tar.gz
sudo docker run --rm --platform linux/amd64 --network none --read-only \
  --cap-drop ALL --security-opt no-new-privileges --memory 2g \
  sipi-benchmark/resize-axis:2026-10-12-amd64 > resize-home.json
```

This runs 15 synthetic workloads (gray/RGB/RGBA, integer/fractional downscaling,
preview, thumbnail and bilinear enlargement), with two warmups and nine measured
iterations per case. Four serial runs use before/after/after/before order.
Progress and a short speedup summary appear in the terminal; raw samples,
comparison, CPU flags visible to the VM, cgroup limits and hashes go to JSON.
Success requires matching output fingerprints across all four runs. A nonzero
exit or empty JSON means failure; do not interpret it as a completed measurement.
A speedup of 1.20 means the operation is 1.20 times as fast, not 20% less time.

The synthetic kernel and Grok codec each use one worker. There is no added CPU
quota (periodic throttling can distort measurements). Other VM or host activity
can still interfere. Prefer a quiet host and repeat the complete command if the
results vary. The VM's CPU model/flags may differ from the physical host; these
results characterize the VM and do not establish general Intel/AMD performance.

Copy the results back from the development Mac:

```sh
scp rosenth@media.home.org:~/resize-home.json build/resize-docker/
```

## Prepared six-pair transfer bundle

The complete corpus can be packaged from the existing local archive preparation:

```sh
python3 benchmarks/package_resize_corpus.py
```

The packager checks all twelve inputs against the recorded ARM comparison hashes
and refuses to overwrite an existing bundle. It includes only `pyramid.tif` and
`image.jp2` for each of the six images, a checksum manifest and a launcher. Original
and full-resolution reference files are omitted. No new Docker image is needed.
The data stays in a separate benchmark directory, outside the media repository.

On the Mac, transfer the prepared bundle and its checksum:

```sh
scp build/resize-docker/resize-archive-inputs.tar.gz \
  build/resize-docker/resize-archive-inputs.tar.gz.sha256 \
  rosenth@media.home.org:~/
```

On the VM, use a fresh directory (plain `mkdir` intentionally refuses an existing
one), verify the transfer, extract and run:

```sh
cd ~
sha256sum -c resize-archive-inputs.tar.gz.sha256
mkdir sipi-resize-archive-test
tar -xzf resize-archive-inputs.tar.gz -C sipi-resize-archive-test
sudo sh sipi-resize-archive-test/sipi-resize-archive/run.sh > resize-home-images.json
```

Stop if a command fails. The launcher validates all input checksums, then passes
all twelve files to the already loaded image, mounted read-only. No server needs
to stop. Allow several minutes, depending on the VM: each of four ABBA runs
contains 15 synthetic cases plus 36 real-image cases, two warmups and nine measured
iterations each (1,836 measured outputs in total). JP2 decoding is the main cost.
For a repeat, reuse the extracted bundle and only run its launcher again, redirecting
to a new result filename. An interrupted/failed run produces no complete JSON.

Copy the completed result back on the Mac:

```sh
scp rosenth@media.home.org:~/resize-home-images.json build/resize-docker/
```

## Optional real-image render core

For comparability, transfer the same prepared TIFF/JP2 corpus described in the
[ARM baseline](2026-10-11-resize.md), rather than substituting unknown repository
masters. Mount a **separate benchmark directory** read-only. For example, with the
prepared `archive-images` directory in the remote user's home:

```sh
sudo docker run --rm --platform linux/amd64 --network none --read-only \
  --cap-drop ALL --security-opt no-new-privileges --memory 2g \
  --mount type=bind,src="$HOME/archive-images",dst=/images,readonly \
  sipi-benchmark/resize-axis:2026-10-12-amd64 \
  /images/pharch_01/pyramid.tif /images/pharch_01/image.jp2 \
  > resize-home-images.json
```

Pass additional pairs explicitly to cover all six images. These runs include the
synthetic cases plus decode/resize/JPEG stage timings for each input. Input hashes
are recorded and checked again after measurement. They exclude HTTP, authorization,
S3 and the result cache; they are not a server-throughput benchmark. Files must be
readable by UID 10001 inside the container.

The reference source and provenance are in `benchmarks/reference/`. It is an
experiment fixture, never linked into the server. The runner is
`benchmarks/compare_resize.py`; its regression checks run with:

```sh
python3 -m unittest discover -s benchmarks -p test_compare_resize.py
```
