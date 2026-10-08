#!/usr/bin/env python3
"""Compare mask/result caches for local and local-MinIO IIIF tile delivery.

Own all listeners, a uniquely named temporary S3 bucket and new output directory.
Uses the existing tile runner's persistent clients and byte-for-byte verification.
Cold means application caches; OS, Docker and MinIO page caches are not flushed.
"""
import argparse
import contextlib
import hashlib
import http.client
import json
import os
from pathlib import Path
import platform
import random
import statistics
import struct
import subprocess
import time
import uuid
import zlib
from tile_load import RssSampler, batch, digest, fetch, jpeg_dimensions, paths_for, rows, server


def write_mask(path):
    """Deterministic untagged gray8 1024x512 synthetic emblem with soft coverage.

    Broad strokes and diagonal lines exercise zero, partial and full coverage.
    This is benchmark input, not a museum logo or representative compression corpus.
    """
    width, height = 1024, 512
    raw = bytearray()
    for y in range(height):
        raw.append(0)  # PNG filter None; libpng still performs decompression/validation.
        for x in range(width):
            edge = min(x, width-1-x, y, height-1-y)
            stroke = ((x+y)//32) % 4 == 0 or abs(y-height//2) < 32
            raw.append(min(255, edge*16) if stroke else 0)

    def chunk(kind, data):
        return struct.pack('>I', len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data))

    path.write_bytes(b'\x89PNG\r\n\x1a\n'
                     +chunk(b'IHDR',struct.pack('>IIBBBBB',width,height,8,0,0,0,0))
                     +chunk(b'IDAT',zlib.compress(raw,6))+chunk(b'IEND',b''))


@contextlib.contextmanager
def minio_sources(images, mask):
    """Upload only to our new bucket; remove our three objects and bucket on exit.

    Deliberately targets the repository's local MinIO test endpoint. Credentials
    can be overridden by benchmark-only environment variables; never serialized.
    """
    bucket = 'sipi-watermark-bench-'+uuid.uuid4().hex
    endpoint = 'http://127.0.0.1:9000'
    access = os.environ.get('SIPI_BENCH_S3_ACCESS_KEY','sipi-test')
    secret = os.environ.get('SIPI_BENCH_S3_SECRET_KEY','sipi-local-test-password')

    def request(method, key='', source=None):
        args = ['curl','--fail','--silent','--show-error','--max-time','120',
                '--aws-sigv4','aws:amz:us-east-1:s3','--user',access+':'+secret,
                '-X',method,endpoint+'/'+bucket+('/'+key if key else '')]
        if source is not None:
            args += ['--upload-file',str(source)]
        result = subprocess.run(args,capture_output=True)
        if result.returncode:
            # Avoid exception strings containing credentials from the command line.
            raise RuntimeError(f'Local MinIO {method} failed with curl status {result.returncode}')

    request('PUT')
    names = ('master.tif','master.jp2','mask.png')
    try:
        for name in names:
            request('PUT',name,mask if name=='mask.png' else images/name)
        yield ['--iiif-storage=s3','--s3-bucket',bucket,'--s3-endpoint',endpoint,
               '--s3-path-style=true','--s3-access-key',access,'--s3-secret-key',secret]
    finally:
        for name in names:
            request('DELETE',name)
        request('DELETE')


def summarize(trials):
    """Median per-trial rates/quantiles, with ranges to expose repeat variability."""
    result=[]
    keys=sorted({(t['source'],t['backend'],t['mask_cache'],t['result_cache'],t['phase'],t.get('implementation','current')) for t in trials})
    for source,backend,mask_cache,result_cache,phase,implementation in keys:
        selected=[t for t in trials if
                  (t['source'],t['backend'],t['mask_cache'],t['result_cache'],t['phase'],t.get('implementation','current'))
                  ==(source,backend,mask_cache,result_cache,phase,implementation)]
        rates=[t['requests_per_second'] for t in selected]
        rss=[t['rss_sampled_peak_bytes']/1048576 for t in selected if t['rss_sampled_peak_bytes'] is not None]
        result.append(dict(implementation=implementation,source=source,backend=backend,mask_cache=mask_cache,result_cache=result_cache,
                           phase=phase,req_s_median=statistics.median(rates),req_s_min=min(rates),req_s_max=max(rates),
                           p50_ms_median=statistics.median(t['p50_ms'] for t in selected),
                           p95_ms_median=statistics.median(t['p95_ms'] for t in selected),
                           rss_peak_mib_median=statistics.median(rss) if rss else None))
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--server',type=Path,required=True)
    parser.add_argument('--images',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True,help='New directory, never overwritten')
    parser.add_argument('--repetitions',type=int,default=3)
    parser.add_argument('--warm-rounds',type=int,default=4)
    parser.add_argument('--clients',type=int,default=8)
    parser.add_argument('--workers',type=int,default=4)
    parser.add_argument('--result-cache-only',action='store_true',help='Longer focused runs of encoded-result hits')
    parser.add_argument('--s3-baseline-server',type=Path,help='Compare old/new binaries: S3 only, mask cache enabled')
    parser.add_argument('--cached-warm-rounds',type=int,help='Separate warm length for enabled result caches')
    args=parser.parse_args()
    if args.cached_warm_rounds is not None and args.cached_warm_rounds<1:
        parser.error('Positive cached warm rounds required')
    if args.repetitions<1 or args.warm_rounds<1 or not 1<=args.clients<=32 or not 1<=args.workers<=32:
        parser.error('Require positive repetitions/rounds and 1..32 clients/workers')
    binary,images,output=(p.resolve() for p in (args.server,args.images,args.output))
    baseline=args.s3_baseline_server.resolve() if args.s3_baseline_server else None
    output.mkdir(parents=True,exist_ok=False)
    mask=output/'mask.png'
    write_mask(mask)
    policies={}
    for backend,source in [('local',json.dumps(str(mask))),('s3',"{type='s3',key='mask.png'}")]:
        policy=output/(backend+'.lua')
        # Warm image headers independently: this synthetic policy marks image delivery only.
        policy.write_text("function preflight(r)\n"
                          "  if r.kind~='image' then return {decision='allow'} end\n"
                          "  return {decision='allow',watermark={source="+source+",\n"
                          "    width_fraction=0.5,opacity=0.25,color={255,255,255}}}\nend\n")
        policies[backend]=policy
    report=dict(complete=False,utc_started=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),
                platform=platform.platform(),python=platform.python_version(),logical_cpus=os.cpu_count(),
                server_sha256=digest(binary),runner_sha256=digest(Path(__file__)),
                client_helper_sha256=digest(Path(__file__).with_name("tile_load.py")),source_sha256={n:digest(images/n) for n in ('master.tif','master.jp2')},
                mask_sha256=digest(mask),mask_encoded_bytes=mask.stat().st_size,mask_decoded_bytes=1024*512,
                clients=args.clients,workers=args.workers,repetitions=args.repetitions,warm_rounds=args.warm_rounds,
                result_cache_only=args.result_cache_only,cached_warm_rounds=args.cached_warm_rounds,
                baseline_sha256=digest(baseline) if baseline else None,
                trials=[])

    def save():
        (output/'results.json').write_text(json.dumps(report,indent=2)+'\n')

    save()
    references={}
    for name in ('master.tif','master.jp2'):
        expected={}
        extra=['--preflight-script',str(policies['local']),'--mask-cache-max-entries=0']
        with server(binary,images,output/(name+'-reference'),1,False,extra) as (port,_):
            with contextlib.closing(http.client.HTTPConnection('127.0.0.1',port,timeout=30)) as conn:
                _,body=fetch(conn,f'/{name}/info.json')
                info=json.loads(body)
                if (info['width'],info['height'])!=(4096,4096):
                    raise RuntimeError('Expected benchmark-images 4096x4096 fixtures')
                for path in paths_for(name):
                    media,body=fetch(conn,path)
                    if not media.startswith('image/jpeg') or jpeg_dimensions(body)!=(256,256):
                        raise RuntimeError('Reference MIME/dimensions differ')
                    expected[path]=hashlib.sha256(body).hexdigest()
        # Prove this policy actually changes a central tile, not just dimensions/MIME.
        central=paths_for(name)[63]
        with server(binary,images,output/(name+'-unmarked-check'),1,False) as (port,_):
            with contextlib.closing(http.client.HTTPConnection('127.0.0.1',port,timeout=30)) as conn:
                _,plain=fetch(conn,central)
                if hashlib.sha256(plain).hexdigest()==expected[central]:
                    raise RuntimeError('Central reference tile was not watermarked')
        references[name]=expected
    report['reference_sha256']=references
    save()
    rng=random.Random(20261008)
    with minio_sources(images,mask) as remote:
        for repetition in range(1,args.repetitions+1):
            result_modes=(True,) if args.result_cache_only else (False,True)
            configs=([(name,'s3',True,rc,label) for name in references for rc in result_modes
                      for label in ('baseline','current')] if baseline else
                     [(name,backend,mc,rc,'current') for name in references for backend in ('local','s3')
                      for mc in (False,True) for rc in result_modes])
            rng.shuffle(configs)
            for name,backend,mc,rc,implementation in configs:
                directory=output/f'r{repetition}-{name}-{backend}-mask{int(mc)}-result{int(rc)}-{implementation}'
                extra=['--preflight-script',str(policies[backend]),'--mask-cache-max-entries',str(64 if mc else 0)]
                if backend=='s3':
                    extra+=remote
                selected_binary=baseline if implementation=='baseline' else binary
                with server(selected_binary,images,directory,args.workers,rc,extra) as (port,pid):
                    with contextlib.closing(http.client.HTTPConnection('127.0.0.1',port,timeout=30)) as conn:
                        fetch(conn,f'/{name}/info.json')  # Image metadata warm; mask and results untouched.
                    if rc and rows(directory)[0]!=0:
                        raise RuntimeError('Expected initially empty result cache')
                    paths=paths_for(name)
                    rounds=args.cached_warm_rounds if rc and args.cached_warm_rounds else args.warm_rounds
                    for phase,workload in [('cold',paths),('warm',paths*rounds)]:
                        with RssSampler(pid) as memory:
                            measured,before,after=batch(port,workload,args.clients,references[name])
                        active=[rss for at,rss in memory.samples if before<=at<=after]
                        measured.update(implementation=implementation,source=name,backend=backend,mask_cache=mc,result_cache=rc,
                                        repetition=repetition,phase=phase,rss_sampled_peak_bytes=max(active) if active else None,
                                        rss_samples=len(active))
                        if rc and rows(directory)[0]!=len(paths):
                            raise RuntimeError('Result cache fill missing or eviction occurred')
                        report['trials'].append(measured)
                        save()
                        print(f'{implementation} {repetition} {name} {backend} mask={mc} result={rc} {phase}: '
                              f'{measured["requests_per_second"]:.1f} req/s, p95={measured["p95_ms"]:.2f} ms',flush=True)
    report['summary']=summarize(report['trials'])
    report['complete']=True
    report['minio_cleanup']='Owned temporary objects and bucket deleted'
    save()
    lines=['# Watermarked tile workload','',
           '| Build | Source | Backend | Mask cache | Result cache | Phase | req/s | range | p95 ms | RSS MiB |',
           '| --- | --- | --- | --- | --- | --- | ---: | --- | ---: | ---: |']
    for row in report['summary']:
        rss=row['rss_peak_mib_median']
        lines.append(f"| {row['implementation']} | {row['source']} | {row['backend']} | {row['mask_cache']} | {row['result_cache']} | {row['phase']} | "
                     f"{row['req_s_median']:.1f} | {row['req_s_min']:.1f}–{row['req_s_max']:.1f} | {row['p95_ms_median']:.2f} | "
                     +(f'{rss:.1f}' if rss is not None else 'not sampled')+' |')
    (output/'summary.md').write_text('\n'.join(lines)+'\n')


if __name__=='__main__':
    main()
