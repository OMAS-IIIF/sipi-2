#!/usr/bin/env python3
"""Compare owned ARM64 Docker listeners on identical pyramidal TIFF tiles.

Requires Pillow (the external-validator venv suffices), Docker, existing images,
and the unmodified Cantaloupe sample properties. No existing service is touched.
Results include every request; warm means warm application and OS caches. This
is a local closed-loop microbenchmark, not a deployment capacity prediction.
"""
import argparse
import contextlib
from concurrent.futures import ThreadPoolExecutor
import hashlib
import http.client
import io
import json
import math
from pathlib import Path
import platform
import random
import statistics
import subprocess
import threading
import time
import uuid

from PIL import Image, ImageChops, ImageStat
from tile_load import digest, fetch, paths_for


def docker(*args):
    """Run Docker with checked status and return text, never invoke a shell."""
    return subprocess.check_output(['docker', *args], text=True).strip()


def configuration(sample, cache, archive=False):
    """Override upstream defaults explicitly; never import OLDAP credentials."""
    values = {
        'http.host': '0.0.0.0', 'http.port': '8182',
        'delegate_script.enabled': 'false', 'source.static': 'FilesystemSource',
        'source.delegate': 'false', 'FilesystemSource.lookup_strategy': 'BasicLookupStrategy',
        'FilesystemSource.BasicLookupStrategy.path_prefix': '/images/',
        'processor.selection_strategy': 'ManualSelectionStrategy',
        'processor.ManualSelectionStrategy.tif': 'Java2dProcessor',
        'processor.jpg.quality': '90', 'processor.jpg.progressive': 'false',
        'cache.client.enabled': 'false', 'cache.server.source': '',
        'cache.server.derivative.enabled': str(cache).lower(),
        'cache.server.derivative': 'FilesystemCache',
        'cache.server.info.enabled': 'true', 'cache.server.resolve_first': 'true',
        'cache.server.worker.enabled': 'false', 'FilesystemCache.pathname': '/cache',
        'log.application.level': 'warn', 'log.access.ConsoleAppender.enabled': 'false',
    }
    if archive:
        values.update({'processor.ManualSelectionStrategy.jp2': 'OpenJpegProcessor',
                       'OpenJpegProcessor.path_to_binaries': '/usr/bin'})
    lines = []
    for line in sample.read_text().splitlines():
        key = line.split('=', 1)[0].strip()
        if key in values:
            lines.append(f'{key} = {values.pop(key)}')
        else:
            lines.append(line)
    lines.extend(f'{key} = {value}' for key, value in values.items())
    return '\n'.join(lines) + '\n'


def triplet_configuration(cached, archive=False):
    """Public file source, four jobs, no libvips operation cache; stock encoding."""
    return f'''server:
  listen: ":8182"
  public_base_url: "http://127.0.0.1:8182"
logging:
  level: warn
vips:
  concurrency: 1
  cache_max_mem: 0
  cache_max_files: 0
  block_untrusted: true
iiif:
  image:
    enabled: true
    prefix: /iiif/3
    max_concurrent_transforms: 4
    max_source_pixels: {300000000 if archive else 250000000}
    max_output_pixels: {4194304 if archive else 100000000}
    max_width: {2048 if archive else 0}
    max_height: {2048 if archive else 0}
    color_management: preserve
    load_access: auto
    info_dimension_cache: true
  presentation:
    enabled: false
sources:
  default: file
  file:
    root: /images
cache:
  root: "{'/cache' if cached else ''}"
  max_bytes: 512MiB
extensions:
  transform:
    enabled: false
  uploads:
    enabled: false
'''


def iip_configuration(cached, archive=False):
    """Four independent FastCGI workers; tile cache is not a response-file cache."""
    return f'''server.modules = ("mod_fastcgi")
server.document-root = "/tmp"
server.port = 8182
server.bind = "0.0.0.0"
server.errorlog = "/dev/stderr"
fastcgi.server = ("/iiif/" => ((
  "socket" => "/tmp/iipsrv.sock",
  "bin-path" => "/usr/sbin/iipsrv",
  "max-procs" => 4,
  "check-local" => "disable",
  "bin-environment" => (
    "FILESYSTEM_PREFIX" => "/images/",
    "URI_MAP" => "iiif=>IIIF",
    "VERBOSITY" => "0",
    "JPEG_QUALITY" => "90",
    "MAX_IMAGE_CACHE_SIZE" => "{128 if cached else 0}",
    "MAX_CVT" => "{2048 if archive else 5000}",
    "OPJ_NUM_THREADS" => "1",
    "OMP_NUM_THREADS" => "1"
  )
)))
'''


@contextlib.contextmanager
def server(kind, image, images, sample, directory, cached, *,
           readiness=("master.tif", 4096, 4096), archive=False, jpeg_options=None):
    """Own one isolated container, retain its logs, remove it even on failure.

    Optional (quality, sampling) applies only to SIPI versions supporting the
    JPEG flags. None preserves historical image defaults and command lines.
    """
    if jpeg_options is not None and kind != 'sipi':
        raise ValueError('JPEG CLI options are specific to SIPI')
    directory.mkdir()
    name = 'sipi-compare-' + uuid.uuid4().hex[:12]
    args = ['run', '-d', '--no-healthcheck', '--name', name, '--platform', 'linux/arm64',
            '--cpus', '4', '--memory', '4g', '--memory-swap', '4g',
            '-p', '127.0.0.1::8182', '--mount', f'type=bind,src={images},dst=/images,readonly',
            '--tmpfs', '/cache:rw,size=536870912,mode=1777']
    if kind == 'sipi':
        args += [image, '--port', '8182', '--iiif-root', '/images',
                 '--iiif-workers', '4', '--log-level', 'warn']
        if jpeg_options is not None:
            quality, sampling = jpeg_options
            args += ['--jpeg-quality', str(quality), '--jpeg-sampling', sampling]
        if archive:
            args += ['--max-width','2048','--max-height','2048','--max-area','4194304']
        if cached:
            args += ['--cache-dir', '/cache', '--cache-max-bytes', '536870912']
    elif kind == 'triplet':
        config = directory / 'triplet.yaml'
        config.write_text(triplet_configuration(cached, archive))
        args += ['--env', 'GOMAXPROCS=4', '--mount',
                 f'type=bind,src={config},dst=/benchmark.yaml,readonly',
                 image, '-config', '/benchmark.yaml']
    elif kind == 'iip':
        config = directory / 'lighttpd.conf'
        config.write_text(iip_configuration(cached, archive))
        args += ['--mount', f'type=bind,src={config},dst=/benchmark.conf,readonly',
                 '--entrypoint', 'lighttpd', image, '-D', '-f', '/benchmark.conf']
    else:
        config = directory / 'cantaloupe.properties'
        config.write_text(configuration(sample, cached, archive))
        args += ['--mount', f'type=bind,src={config},dst=/benchmark.properties,readonly',
                 '--entrypoint', 'java', image, '-XX:ActiveProcessorCount=4', '-Xmx2g',
                 '-Dcantaloupe.config=/benchmark.properties', '-jar', '/opt/iiif/cantaloupe.jar']
    (directory / 'command.json').write_text(json.dumps(['docker', *args], indent=2))
    try:
        docker(*args)
        port = int(docker('port', name, '8182/tcp').rsplit(':', 1)[1])
        prefix = {'cantaloupe': '/iiif/3', 'sipi': '', 'iip': '/iiif', 'triplet': '/iiif/3'}[kind]
        for _ in range(120):
            try:
                with contextlib.closing(http.client.HTTPConnection('127.0.0.1', port, timeout=2)) as conn:
                    _, body = fetch(conn, prefix + '/' + readiness[0] + '/info.json')
                    info = json.loads(body)
                    if (info['width'], info['height']) != tuple(readiness[1:]):
                        raise ValueError('Readiness image dimensions differ')
                break
            except (OSError, http.client.HTTPException, RuntimeError):
                time.sleep(.5)
        else:
            raise RuntimeError(f'Startup failed: {directory}')
        yield port, prefix, name
    finally:
        logs = subprocess.run(['docker', 'logs', name], capture_output=True, text=True)
        (directory / 'server.log').write_text(logs.stdout + logs.stderr)
        subprocess.run(['docker', 'rm', '-f', name], stdout=subprocess.DEVNULL, check=False)


def batch(port, paths, clients, expected, minimum_seconds=0, *, balanced=False, record_paths=False):
    """Persistent clients; latency includes full body, excludes hashing/queueing.

    balanced makes each client finish complete workload rounds, retaining the
    same image/request mix even when clients complete different round counts.
    record_paths adds request identity for per-image latency diagnostics.
    """
    barrier = threading.Barrier(clients, timeout=60)
    starts = []
    def client(index):
        records = []
        with contextlib.closing(http.client.HTTPConnection('127.0.0.1', port, timeout=60)) as conn:
            conn.connect()
            barrier.wait()
            starts.append(time.perf_counter())
            deadline = time.perf_counter() + minimum_seconds
            while True:
                assigned = paths[index:] + paths[:index] if balanced else paths[index::clients]
                for path in assigned:
                    before = time.perf_counter()
                    media, body = fetch(conn, path)
                    elapsed = time.perf_counter() - before
                    if (media.split(';')[0], hashlib.sha256(body).hexdigest()) != expected[path]:
                        raise RuntimeError(f'Response changed: {path}')
                    records.append([elapsed * 1000, len(body), path] if record_paths else [elapsed * 1000, len(body)])
                if time.perf_counter() >= deadline:
                    break
        return records
    with ThreadPoolExecutor(max_workers=clients) as pool:
        records = [r for group in pool.map(client, range(clients)) for r in group]
    elapsed = time.perf_counter() - min(starts)
    latencies = sorted(r[0] for r in records)
    return {'seconds': elapsed, 'requests': len(records), 'rps': len(records) / elapsed,
            'p50_ms': statistics.median(latencies), 'p95_ms': latencies[math.ceil(.95*len(latencies))-1],
            'bytes': sum(r[1] for r in records), 'records_ms_bytes': records}


def references(port, prefix, source, directory, kind):
    """Decode all serial outputs; PNG must exactly match source pyramid crops.

    Triplet resamples 1:2 from the main TIFF image: record its pixel differences
    instead of requiring the precomputed pyramid averaging kernel. Native 1:1
    PNG crops still must match exactly on every server.
    JPEG differences are measured, not equated: encoders/subsampling can differ.
    Byte hashes check repeatability within a server, never between encoders.
    """
    expected, quality = {}, []
    with Image.open(source) as master:
        levels = {}
        for frame in range(master.n_frames):
            master.seek(frame)
            if master.width in (4096, 2048):
                levels[4096 // master.width] = master.convert('RGB')
    with contextlib.closing(http.client.HTTPConnection('127.0.0.1', port, timeout=60)) as conn:
        for extension in ('jpg', 'png'):
            for path in paths_for('master.tif'):
                path = path.removesuffix('jpg') + extension
                region = list(map(int, path.split('/')[2].split(',')))
                x, y, width, _ = region
                scale = width // 256
                crop = levels[scale].crop((x//scale, y//scale, x//scale+256, y//scale+256))
                media, body = fetch(conn, prefix + path)
                if media.split(';')[0] != {'jpg': 'image/jpeg', 'png': 'image/png'}[extension]:
                    raise RuntimeError(f'Wrong MIME type: {path}')
                with Image.open(io.BytesIO(body)) as output:
                    output.load()
                    if output.size != (256, 256) or output.format != {'jpg':'JPEG', 'png':'PNG'}[extension]:
                        raise RuntimeError('Wrong decoded output')
                    difference = ImageChops.difference(crop, output.convert('RGB'))
                    error = ImageStat.Stat(difference)
                    if extension == 'png' and difference.getbbox() is not None and not (kind == 'triplet' and scale == 2):
                        raise RuntimeError(f'PNG differs from source pyramid: {path}, MAE={error.mean}')
                    quality.append({'path': path, 'bytes': len(body), 'mae_rgb': error.mean,
                                    'rms_rgb': error.rms,
                                    'sampling': getattr(output, 'layer', None)})
                expected[prefix + path] = (media.split(';')[0], hashlib.sha256(body).hexdigest())
    (directory / 'references.json').write_text(json.dumps({'quality': quality, 'hashes': expected}, indent=2))
    return expected


def summarize(report, output):
    """Retain trial aggregates, ranges and one verified reference set per server.

    The large per-request arrays stay in results.json. All repetitions must be
    complete; every cache mode/repetition must retain the same reference bytes.
    """
    if not report['complete']:
        raise ValueError('Cannot summarize an incomplete run')
    compact = {key: value for key, value in report.items() if key != 'trials'}
    compact['raw_results_sha256'] = digest(output / 'results.json')
    compact['trials'] = [{key: value for key, value in trial.items()
                          if key != 'records_ms_bytes'} for trial in report['trials']]
    compact['groups'] = []
    for server_name in report['images']:
        for cached in (False, True):
            for ext in ('jpg', 'png'):
                for clients in (1, 8):
                    trials = [t for t in report['trials'] if
                              (t['server'], t['cached'], t['extension'], t['clients']) ==
                              (server_name, cached, ext, clients)]
                    if len(trials) != report['options']['repetitions']:
                        raise ValueError('Missing or duplicate trials')
                    row = dict(server=server_name, cached=cached, extension=ext, clients=clients)
                    for metric in ('rps', 'p50_ms', 'p95_ms'):
                        values = [t[metric] for t in trials]
                        row[metric] = dict(median=statistics.median(values), min=min(values), max=max(values))
                    compact['groups'].append(row)
    compact['references'] = {}
    for server_name in report['images']:
        for reference in sorted(output.glob(f'*-{server_name}-*/references.json')):
            data = json.loads(reference.read_text())
            for path, (mime, _) in data['hashes'].items():
                if mime != ('image/jpeg' if path.endswith('.jpg') else 'image/png'):
                    raise ValueError('Wrong reference MIME')
            previous = compact['references'].setdefault(server_name, data)
            if previous != data:
                raise ValueError(f'Reference differs across trials: {server_name}')
    (output / 'summary.json').write_text(json.dumps(compact, indent=2) + '\n')
    return compact


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--images', type=Path, required=True)
    parser.add_argument('--sample', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--sipi-image', default='lrosenth/sipi-2:verify-0.1.0-arm64')
    parser.add_argument('--cantaloupe-image', default='lrosenth/oldap-imageserver:latest')
    parser.add_argument('--triplet-image', help='Optional native Triplet image')
    parser.add_argument('--iip-image', help='Optional native IIPImage image; four FastCGI processes')
    parser.add_argument('--minimum-seconds', type=float, default=5)
    parser.add_argument('--repetitions', type=int, default=3)
    parser.add_argument('--warmup-seconds', type=float, default=15)
    parser.add_argument('--render-rounds', type=int, default=4)
    parser.add_argument('--hit-rounds', type=int, default=16)
    options = parser.parse_args()
    if min(options.repetitions, options.render_rounds, options.hit_rounds, options.warmup_seconds, options.minimum_seconds) <= 0:
        parser.error('Counts and warmup must be positive')
    images, sample, output = (p.resolve() for p in (options.images, options.sample, options.output))
    output.mkdir(parents=True, exist_ok=False)
    if docker('info', '--format', '{{.Architecture}}') not in ('aarch64', 'arm64'):
        raise RuntimeError('This benchmark requires a native ARM64 Docker engine')
    pinned = {}
    candidates = [('sipi', options.sipi_image), ('cantaloupe', options.cantaloupe_image)]
    if options.triplet_image:
        candidates.append(('triplet', options.triplet_image))
    if options.iip_image:
        candidates.append(('iip', options.iip_image))
    for kind, tag in candidates:
        data = json.loads(docker('image', 'inspect', tag))[0]
        if data['Architecture'] != 'arm64':
            raise RuntimeError(f'{tag} must be native arm64')
        pinned[kind] = data['Id']
    report = {'complete': False, 'options': vars(options) | {'images': str(images), 'sample': str(sample), 'output': str(output)},
              'images': pinned, 'host': platform.platform(), 'docker': json.loads(docker('info', '--format', '{{json .}}')),
              'source_sha256': digest(images/'master.tif'), 'sample_sha256': digest(sample),
              'runner_sha256': digest(Path(__file__)),
              'started_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'trials': []}
    # Docker info contains local system metadata: keep only facts needed for reproduction.
    report['docker'] = {key: report['docker'][key] for key in ('ServerVersion', 'Architecture', 'NCPU', 'MemTotal', 'KernelVersion')}
    def save():
        (output/'results.json').write_text(json.dumps(report, indent=2)+'\n')
    save()
    (output / 'runner.py').write_bytes(Path(__file__).read_bytes())
    rng = random.Random(20261008)
    for repetition in range(options.repetitions):
        cases = [(kind, cached) for kind in pinned for cached in (False, True)]
        rng.shuffle(cases)
        for kind, cached in cases:
            directory = output / f'{repetition}-{kind}-{"hits" if cached else "render"}'
            print(f'Starting {directory.name}', flush=True)
            with server(kind, pinned[kind], images, sample, directory, cached) as (port, prefix, name):
                expected = references(port, prefix, images/'master.tif', directory, kind)
                paths = list(expected)
                # Timed warmup after every URL was generated; warms JIT and both formats.
                until = time.perf_counter() + options.warmup_seconds
                while time.perf_counter() < until:
                    batch(port, paths, 8, expected)
                workloads = [(ext, clients) for ext in ('jpg', 'png') for clients in (1, 8)]
                rng.shuffle(workloads)
                for ext, clients in workloads:
                    paths = [path for path in expected if path.endswith(ext)]
                    paths *= options.hit_rounds if cached else options.render_rounds
                    rng.shuffle(paths)
                    result = batch(port, paths, clients, expected, options.minimum_seconds)
                    result.update(server=kind, cached=cached, extension=ext, clients=clients, repetition=repetition)
                    report['trials'].append(result)
                    save()
                    print(f'  {ext} c={clients}: {result["rps"]:.1f} req/s, p95={result["p95_ms"]:.1f} ms', flush=True)
                # Diagnostic only, not an apples-to-apples RSS/peak memory measurement.
                (directory/'docker-stats.json').write_text(docker('stats', '--no-stream', '--format', '{{json .}}', name))
    report['finished_utc'] = time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())
    report['complete'] = True
    save()
    summarize(report, output)


if __name__ == '__main__':
    main()
