#!/usr/bin/env python3
"""Prepare lossless HT archive counterparts and measure TIFF/JP2/JPH render cores.

Requires the existing prepared archive corpus and Pillow environment used by
prepare_archives.py. Never modifies originals. Output directory must be new.
Every pixel and ICC is checked by the production reader outside timed work;
this does not constitute an independent HT decoder conformance test.
"""
import argparse
import json
from pathlib import Path
import platform
import statistics
import subprocess
import sys

from prepare_archives import file_sha, jp2_header

ROOT = Path(__file__).resolve().parents[1]


def validate_coding(classic, ht):
    """Reject accidental classic encoding or unequal wavelet/colour settings."""
    if ht['brand'] != 'jph ' or ht['block_style'] != 64 or not ht['rsiz'] & 0x4000 or 'cap' not in ht:
        raise ValueError('Expected JPH with HT coding and capability markers')
    for key in ('bits', 'signed', 'tile_size', 'progression', 'layers', 'mct',
                'resolutions', 'codeblock', 'reversible_5_3', 'colour_method', 'icc_sha256'):
        if classic[key] != ht[key]:
            raise ValueError(f'Unequal JP2/JPH setting: {key}')


def validate_runs(runs, names):
    """Check reproducibility and exact JP2/JPH render equivalence in every case."""
    fingerprints = {}
    for run in runs:
        if len(run['cases']) != 15 + len(names)*9:
            raise ValueError('Incomplete benchmark cases')
        indexed = {}
        for case in run['cases']:
            key = (case['kind'], case.get('file'), case['case'], case['channels'])
            value = (case['fingerprint'], case.get('jpeg_fingerprint'))
            if key in indexed or (key in fingerprints and fingerprints[key] != value):
                raise ValueError('Duplicate or unstable benchmark result')
            indexed[key] = value
            fingerprints[key] = value
        for name in names:
            for request in ('thumbnail', 'preview', 'tile'):
                pair = [c for c in run['cases'] if c['kind']=='pipeline' and
                        Path(c['file']).parent.name == name and c['case']==request and
                        Path(c['file']).suffix in ('.jp2', '.jph')]
                if len(pair) != 2:
                    raise ValueError('Missing codec pair')
                for field in ('fingerprint', 'jpeg_fingerprint', 'output_width', 'output_height',
                              'level_width', 'level_height', 'decoded_width', 'decoded_height'):
                    if pair[0][field] != pair[1][field]:
                        raise ValueError(f'JP2/JPH mismatch: {name}/{request}/{field}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--helper', type=Path, default=ROOT/'build/release/prepare-archive-image')
    parser.add_argument('--benchmark', type=Path, default=ROOT/'build/release/resize-benchmark')
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.output.mkdir()  # Deliberately exclusive; a failed run remains available for diagnosis.
    previous = json.loads((ROOT/'docs/benchmarks/2026-10-12-axis-weights.json').read_text())
    inputs = {str(ROOT/p): sha for p, sha in previous['inputs_sha256'].items()}
    for p, sha in inputs.items():
        if file_sha(Path(p)) != sha:
            raise ValueError(f'Input changed since original comparison: {p}')
    names = sorted({Path(p).parent.name for p in inputs})
    result = {'complete': False, 'environment': {'platform': platform.platform(),
              'machine': platform.machine(), 'python': sys.version},
              'binary_sha256': {str(p): file_sha(p) for p in (args.helper, args.benchmark)},
              'source_sha256': {p: file_sha(ROOT/p) for p in (
                  'benchmarks/PrepareArchiveImage.cpp', 'benchmarks/ResizeBenchmark.cpp',
                  'benchmarks/htj2k_compare.py', 'benchmarks/prepare_archives.py', 'conan.lock')},
              'inputs_sha256': inputs, 'images': [], 'runs': []}
    report = args.output/'report.json'
    def save():
        report.write_text(json.dumps(result, indent=2)+'\n')
    save()
    paths = []
    for name in names:
        print(f'Preparing and verifying {name}', flush=True)
        source = ROOT/'build/archive-images'/name
        reference = source/'reference.tif'
        reference_hash = file_sha(reference)
        target = args.output/name
        with (args.output/(name+'.log')).open('w') as log:
            subprocess.run([str(args.helper), '--htj2k-reference', str(reference), str(target)],
                           check=True, stdout=log, stderr=subprocess.STDOUT)
            subprocess.run([str(args.helper), '--verify-htj2k-reference', str(reference), str(target/'image.jph')],
                           check=True, stdout=log, stderr=subprocess.STDOUT)
        if file_sha(reference) != reference_hash:
            raise ValueError('Reference changed during preparation')
        classic, ht = jp2_header(source/'image.jp2'), jp2_header(target/'image.jph')
        validate_coding(classic, ht)
        triple = [source/'pyramid.tif', source/'image.jp2', target/'image.jph']
        result['images'].append({'name': name, 'reference_sha256': reference_hash,
            'all_pixels_and_icc_verified': True, 'jp2_header': classic, 'jph_header': ht,
            'files': {p.suffix: {'path': str(p), 'bytes': p.stat().st_size, 'sha256': file_sha(p)} for p in triple}})
        paths.extend(str(p) for p in triple)
        save()
    # Preparation/verification is complete before either timed run begins.
    for order in (paths, list(reversed(paths))):
        print('Measuring forward' if order==paths else 'Measuring reverse', flush=True)
        raw = subprocess.run([str(args.benchmark), '9', *order], check=True, capture_output=True, text=True)
        result['runs'].append(json.loads(raw.stdout)); save()
    validate_runs(result['runs'], names)
    result['summary'] = []
    for suffix in ('.tif','.jp2','.jph'):
        for request in ('thumbnail','preview','tile'):
            stages = {stage: statistics.mean(t for run in result['runs'] for c in run['cases']
                if c.get('file','').endswith(suffix) and c['case']==request for t in c[stage]['samples_ms'])
                for stage in ('decode','resize','encode','render_core')}
            result['summary'].append({'format': suffix, 'request': request, **stages})
    for image in result['images']:
        for info in image['files'].values():
            if file_sha(Path(info['path'])) != info['sha256']:
                raise ValueError('Input changed during measurement')
    result['complete'] = True; save()
    print(json.dumps(result['summary'], indent=2))


if __name__ == '__main__':
    main()
