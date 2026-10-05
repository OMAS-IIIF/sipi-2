#!/usr/bin/env python3
"""Measure a fixed IIIF tile workload with correctness checks, without dependencies.

Local loopback HTTP, persistent client connections, fixed closed-loop concurrency.
Cold means empty encoded-result cache, not cold OS caches. Never changes sources,
flushes OS caches or attaches to an existing server. Owns disposable listeners.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import contextlib
import hashlib
import http.client
import json
import math
import os
from pathlib import Path
import platform
import random
import socket
import sqlite3
import statistics
import subprocess
import threading
import time


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def paths_for(identifier):
    """8x8 adjacent tiles at 1:1 and 1:2; aligned 256px output, 128 unique URLs."""
    return [f'/{identifier}/{x * 256 * scale},{y * 256 * scale},{256 * scale},{256 * scale}/256,256/0/default.jpg'
            for scale in (1, 2) for y in range(8) for x in range(8)]


def jpeg_dimensions(data):
    """Read baseline JPEG SOF dimensions to validate the serial reference tiles."""
    if data[:2] != b'\xff\xd8':
        raise RuntimeError('Missing JPEG start marker')
    offset = 2
    while offset + 8 < len(data):
        if data[offset] != 255:
            break
        marker = data[offset + 1]
        length = int.from_bytes(data[offset + 2:offset + 4], 'big')
        if marker == 0xc0:
            return (int.from_bytes(data[offset + 7:offset + 9], 'big'),
                    int.from_bytes(data[offset + 5:offset + 7], 'big'))
        if length < 2:
            break
        offset += length + 2
    raise RuntimeError('Missing baseline JPEG frame')


def fetch(connection, path):
    connection.request('GET', path)
    response = connection.getresponse()
    body = response.read()
    if response.status != 200:
        raise RuntimeError(f'{path}: HTTP {response.status}: {body[:200]!r}')
    return response.getheader('Content-Type', ''), body


class RssSampler:
    """Sample server RSS via ps every 50ms. A sampled lower bound, not peak allocation.

    ps reports KiB on macOS/Linux. Samples include allocator-retained pages. The
    sampling subprocess adds measurement overhead equally to all configurations.
    """
    def __init__(self, pid):
        self.pid = pid
        self.samples = []
        self.done = threading.Event()
        self.thread = threading.Thread(target=self.run)

    def run(self):
        while not self.done.is_set():
            sampled_at = time.perf_counter()
            result = subprocess.run(['ps', '-o', 'rss=', '-p', str(self.pid)],
                                    capture_output=True, text=True, check=False)
            if result.returncode == 0 and result.stdout.strip():
                self.samples.append((sampled_at, int(result.stdout.strip()) * 1024))
            self.done.wait(.05)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *_):
        self.done.set()
        self.thread.join()


@contextlib.contextmanager
def server(binary, images, directory, workers, cache):
    """Run only our child; retain its log and require clean shutdown."""
    directory.mkdir()
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        port = sock.getsockname()[1]
    args = [str(binary), '--port', str(port), '--iiif-root', str(images),
            '--iiif-workers', str(workers), '--log-level', 'warn']
    if cache:
        args += ['--cache-dir', str(directory / 'cache'), '--cache-max-bytes', '536870912']
    env = {k: v for k, v in os.environ.items() if not k.startswith('SIPI_')}
    with (directory / 'server.log').open('w') as log:
        process = subprocess.Popen(args, cwd=directory, env=env, stdout=log, stderr=log)
        try:
            for _ in range(200):
                if process.poll() is not None:
                    raise RuntimeError(f'Server exited; see {directory / "server.log"}')
                try:
                    with contextlib.closing(http.client.HTTPConnection('127.0.0.1', port, timeout=1)) as conn:
                        fetch(conn, '/health')
                    break
                except OSError:
                    time.sleep(.025)
            else:
                raise RuntimeError('Server startup timeout')
            yield port, process.pid
        finally:
            if process.poll() is None:
                process.terminate()
            try:
                process.wait(timeout=30)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
                raise RuntimeError('Server failed to stop within 30 seconds')
            if process.returncode != 0:
                raise RuntimeError(f'Server exited with {process.returncode}; see {directory / "server.log"}')


def batch(port, paths, clients, expected):
    """Each client owns one connection; latency excludes client queue/connect time.

    Wall time includes body hashing/comparison between requests. No latency or
    throughput pass threshold: only status, MIME and exact response bytes gate.
    """
    start = []
    barrier = threading.Barrier(clients, action=lambda: start.append(time.perf_counter()), timeout=30)

    def client(index):
        records = []
        with contextlib.closing(http.client.HTTPConnection('127.0.0.1', port, timeout=30)) as conn:
            conn.connect()
            barrier.wait()
            for path in paths[index::clients]:
                before = time.perf_counter()
                media, body = fetch(conn, path)
                after = time.perf_counter()
                checksum = hashlib.sha256(body).hexdigest()
                if not media.startswith('image/jpeg') or checksum != expected[path]:
                    raise RuntimeError(f'Output differs from serial reference: {path}')
                records.append({'path': path, 'latency_ms': (after - before) * 1000, 'bytes': len(body)})
        return records

    with ThreadPoolExecutor(max_workers=clients) as executor:
        results = list(executor.map(client, range(clients)))
    end = time.perf_counter()
    records = [record for group in results for record in group]
    latencies = sorted(record['latency_ms'] for record in records)
    elapsed = end - start[0]
    return {'elapsed_seconds': elapsed, 'requests': len(records),
            'requests_per_second': len(records) / elapsed,
            'p50_ms': statistics.median(latencies),
            'p95_ms': latencies[math.ceil(.95 * len(latencies)) - 1],
            'bytes': sum(record['bytes'] for record in records), 'records': records}, start[0], end


def rows(directory):
    with sqlite3.connect(f'file:{directory / "cache" / "cache.sqlite3"}?mode=ro', uri=True) as db:
        return db.execute('SELECT count(*),sum(bytes) FROM entries').fetchone()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--server', type=Path, required=True)
    parser.add_argument('--images', type=Path, required=True, help='Directory generated by benchmark-images')
    parser.add_argument('--output', type=Path, required=True, help='New results directory; never overwrite')
    parser.add_argument('--repetitions', type=int, default=5)
    parser.add_argument('--clients', type=int, default=8)
    parser.add_argument('--warm-rounds', type=int, default=128)
    options = parser.parse_args()
    if options.repetitions < 1 or not 1 <= options.clients <= 32 or options.warm_rounds < 1:
        parser.error('Require positive repetitions/rounds and 1..32 clients')
    binary, images, output = (p.resolve() for p in (options.server, options.images, options.output))
    output.mkdir(parents=True, exist_ok=False)
    report = {'complete': False, 'utc_started': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
              'platform': platform.platform(), 'python': platform.python_version(),
              'logical_cpus': os.cpu_count(), 'server_sha256': digest(binary),
              'source_sha256': {name: digest(images / name) for name in ('master.tif', 'master.jp2')},
              'clients': options.clients, 'warm_rounds': options.warm_rounds,
              'repetitions': options.repetitions, 'trials': []}

    def save():
        (output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')

    save()
    randomizer = random.Random(20261006)
    for name in ('master.tif', 'master.jp2'):
        paths = paths_for(name)
        expected = {}
        with server(binary, images, output / (name + '-reference'), 1, False) as (port, _):
            with contextlib.closing(http.client.HTTPConnection('127.0.0.1', port, timeout=30)) as conn:
                _, body = fetch(conn, f'/{name}/info.json')
                info = json.loads(body)
                if (info['width'], info['height']) != (4096, 4096):
                    raise RuntimeError('Benchmark requires the generated 4096x4096 fixtures')
                for path in paths:
                    media, body = fetch(conn, path)
                    if not media.startswith('image/jpeg') or jpeg_dimensions(body) != (256, 256):
                        raise RuntimeError('Invalid serial reference MIME/dimensions')
                    expected[path] = hashlib.sha256(body).hexdigest()
        for repetition in range(options.repetitions):
            workers = [1, 2, 4]
            randomizer.shuffle(workers)
            for count in workers:
                directory = output / f'{name}-r{repetition + 1}-w{count}'
                with server(binary, images, directory, count, True) as (port, pid):
                    # Warm metadata deliberately: isolate result-cache/render behavior.
                    with contextlib.closing(http.client.HTTPConnection('127.0.0.1', port, timeout=30)) as conn:
                        fetch(conn, f'/{name}/info.json')
                    assert rows(directory)[0] == 0
                    for phase, workload in [('cold', paths), ('warm', paths * options.warm_rounds)]:
                        with RssSampler(pid) as memory:
                            result, before, after = batch(port, workload, options.clients, expected)
                        active = [rss for at, rss in memory.samples if before <= at <= after]
                        result.update({'source': name, 'repetition': repetition + 1, 'workers': count,
                                       'phase': phase, 'rss_sampled_peak_bytes': max(active) if active else None,
                                       'rss_samples': len(active)})
                        if rows(directory)[0] != len(paths):
                            raise RuntimeError('Unexpected cache entry count: fill missing or eviction occurred')
                        report['trials'].append(result)
                        save()
                        print(f'{name} repeat={repetition + 1} workers={count} {phase}: '
                              f'{result["requests_per_second"]:.1f} req/s, '
                              f'p95={result["p95_ms"]:.2f} ms, RSS samples={len(active)}', flush=True)
    summaries = []
    for source in ('master.tif', 'master.jp2'):
        for phase in ('cold', 'warm'):
            for workers in (1, 2, 4):
                trials = [t for t in report['trials'] if
                          (t['source'], t['phase'], t['workers']) == (source, phase, workers)]
                rates = [t['requests_per_second'] for t in trials]
                rss = [t['rss_sampled_peak_bytes'] / (1024 * 1024) for t in trials
                       if t['rss_sampled_peak_bytes'] is not None]
                summaries.append({'source': source, 'phase': phase, 'workers': workers,
                    'req_s_median': statistics.median(rates), 'req_s_min': min(rates), 'req_s_max': max(rates),
                    'p50_ms_median': statistics.median(t['p50_ms'] for t in trials),
                    'p95_ms_median': statistics.median(t['p95_ms'] for t in trials),
                    'rss_sampled_peak_mib_median': statistics.median(rss) if rss else None})
    report['summary'] = summaries
    lines = ['# Local tile workload', '',
             'Medians across repetitions; RSS is sampled, not an allocation limit.', '',
             '| Source | Cache | Workers | req/s | req/s range | p50 ms | p95 ms | RSS MiB |',
             '| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |']
    for row in summaries:
        rss = row['rss_sampled_peak_mib_median']
        lines.append(f"| {row['source']} | {row['phase']} | {row['workers']} | "
                     f"{row['req_s_median']:.1f} | {row['req_s_min']:.1f}–{row['req_s_max']:.1f} | "
                     f"{row['p50_ms_median']:.2f} | {row['p95_ms_median']:.2f} | "
                     + (f'{rss:.1f}' if rss is not None else 'not sampled') + ' |')
    (output / 'summary.md').write_text('\n'.join(lines) + '\n')
    report['complete'] = True
    report['reference_checksums'] = 'Every measured response matched the serial reference for its source and URL.'
    save()


if __name__ == '__main__':
    main()
