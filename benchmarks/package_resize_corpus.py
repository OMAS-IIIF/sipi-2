#!/usr/bin/env python3
"""Package exactly the twelve prepared inputs from the recorded ARM comparison.

Run from any directory. Verifies original hashes before creating an exclusive
output archive; originals/reference rasters are never included or modified.
Requires only Python's standard library. An existing archive is never overwritten.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import tarfile

ROOT = Path(__file__).resolve().parents[1]
PREFIX = Path('build/archive-images')


def digest(path):
    """Return a streaming SHA-256 without retaining the image in memory."""
    sha = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            sha.update(block)
    return sha.hexdigest()


def main():
    """Validate the corpus, then write a portable archive and host checksum."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path,
                        default=ROOT / 'build/resize-docker/resize-archive-inputs.tar.gz')
    args = parser.parse_args()
    manifest = json.loads((ROOT / 'docs/benchmarks/2026-10-12-axis-weights.json').read_text())
    files = []
    for name, expected in manifest['inputs_sha256'].items():
        relative = Path(name).relative_to(PREFIX)
        if '..' in relative.parts or relative.name not in ('pyramid.tif', 'image.jp2'):
            raise ValueError(f'Unexpected corpus path: {name}')
        source = ROOT / name
        if digest(source) != expected:
            raise ValueError(f'Input differs from recorded baseline: {name}')
        files.append((source, relative, expected))
    if len(files) != 12 or len({p.parent for _, p, _ in files}) != 6:
        raise ValueError('Expected six TIFF/JP2 pairs')
    checksums = ''.join(f'{sha}  images/{rel.as_posix()}\n' for _, rel, sha in files)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open('xb') as destination:
        with tarfile.open(fileobj=destination, mode='w:gz', compresslevel=1) as archive:
            for source, relative, _ in files:
                info = tarfile.TarInfo(f'sipi-resize-archive/images/{relative.as_posix()}')
                info.size = source.stat().st_size
                info.mode = 0o644
                with source.open('rb') as stream:
                    archive.addfile(info, stream)
            for name, data in (
                ('SHA256SUMS', checksums.encode()),
                ('run.sh', (ROOT / 'benchmarks/run_resize_archive.sh').read_bytes()),
            ):
                info = tarfile.TarInfo(f'sipi-resize-archive/{name}')
                info.size, info.mode = len(data), 0o644
                archive.addfile(info, io.BytesIO(data))
    args.output.with_suffix(args.output.suffix + '.sha256').write_text(
        f'{digest(args.output)}  {args.output.name}\n')
    print(f'{args.output}: {len(files)} files, {args.output.stat().st_size / 1024**2:.1f} MiB')


if __name__ == '__main__':
    main()
