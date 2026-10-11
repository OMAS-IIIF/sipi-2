#!/usr/bin/env python3
"""Run a serial ABBA resize comparison; emit one self-contained JSON report.

Optional TIFF/JP2 inputs are read only. Timings exclude fingerprint verification.
A mismatch or failed child aborts with nonzero status; stdout then has no report.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys


def sha256(path):
    """Hash a file in bounded chunks, outside measured work."""
    digest = hashlib.sha256()
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def case_key(case):
    """Identify synthetic layouts and real-file requests independently of order."""
    return (case['kind'], case.get('file', ''), case['case'], case['channels'])


def compare(runs):
    """Validate case sets/output fingerprints, then compare paired median timings."""
    indexed = []
    for run in runs:
        cases = {case_key(c): c for c in run['report']['cases']}
        if len(cases) != len(run['report']['cases']):
            raise ValueError('Duplicate benchmark case')
        indexed.append(cases)
    if not indexed or not indexed[0] or any(set(c) != set(indexed[0]) for c in indexed):
        raise ValueError('Benchmark case sets differ or are empty')
    result = []
    for key in sorted(indexed[0]):
        cases = [c[key] for c in indexed]
        for field in ('fingerprint', 'jpeg_fingerprint', 'jpeg_bytes',
                      'output_width', 'output_height', 'source_width', 'source_height'):
            if any(c.get(field) != cases[0].get(field) for c in cases):
                raise ValueError(f'Output mismatch: {key}, {field}')
        entry = dict(zip(('kind', 'file', 'case', 'channels'), key))
        for stage in ('resize', 'decode', 'encode', 'render_core'):
            if stage not in cases[0]:
                continue
            before = statistics.mean(c[stage]['median_ms'] for r, c in zip(runs, cases)
                                     if r['variant'] == 'before')
            after = statistics.mean(c[stage]['median_ms'] for r, c in zip(runs, cases)
                                    if r['variant'] == 'after')
            entry[stage] = {'before_ms': before, 'after_ms': after,
                            'speedup': before / after}
        result.append(entry)
    return result


def main():
    """Collect provenance, execute four runs sequentially, and validate results."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repetitions', type=int, default=9)
    parser.add_argument('--binary-dir', type=Path, default=Path('/opt/benchmark'))
    parser.add_argument('images', type=Path, nargs='*')
    args = parser.parse_args()
    if args.repetitions < 3 or args.repetitions > 101 or args.repetitions % 2 == 0:
        parser.error('--repetitions must be odd, in 3..101')
    files = [str(p.resolve(strict=True)) for p in args.images]
    if len(set(files)) != len(files):
        parser.error('Duplicate input paths')
    inputs = {p: sha256(p) for p in files}
    binaries = {v: args.binary_dir / v for v in ('before', 'after')}
    metadata = {}
    for path in ('/proc/cpuinfo', '/sys/fs/cgroup/cpu.max', '/sys/fs/cgroup/cpuset.cpus.effective',
                 '/sys/fs/cgroup/memory.max'):
        if Path(path).is_file():
            metadata[path] = Path(path).read_text()
    manifest = args.binary_dir / 'source-sha256.txt'
    report = {'schema': 1, 'machine': platform.machine(), 'platform': platform.platform(),
              'cpu_count': os.cpu_count(), 'system': metadata, 'input_sha256': inputs,
              'binary_sha256': {v: sha256(p) for v, p in binaries.items()},
              'source_manifest': manifest.read_text() if manifest.exists() else None,
              'runs': []}
    for i, variant in enumerate(('before', 'after', 'after', 'before')):
        print(f'Run {i+1}/4: {variant}', file=sys.stderr, flush=True)
        order = files if i < 2 else list(reversed(files))
        child = subprocess.run([str(binaries[variant]), str(args.repetitions), *order],
                               check=True, stdout=subprocess.PIPE, text=True)
        report['runs'].append({'variant': variant, 'report': json.loads(child.stdout)})
    report['comparison'] = compare(report['runs'])
    if any(sha256(p) != digest for p, digest in inputs.items()):
        raise ValueError('Input changed during benchmark')
    print('All output fingerprints match. Speedups (before / after):', file=sys.stderr)
    for case in report['comparison']:
        print(f"  {case['case']} ch={case['channels']} {case['file']}: "
              f"resize {case['resize']['speedup']:.2f}x", file=sys.stderr)
    json.dump(report, sys.stdout, indent=2)
    print()


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f'Benchmark failed: {error}', file=sys.stderr)
        sys.exit(1)
