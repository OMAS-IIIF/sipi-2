#!/usr/bin/env python3
"""Profile S3 operation counts, bounded payload bytes and SDK/validation duration.

Uses disposable local MinIO data and the existing watermarked tile workload.
Debug logs are diagnostic evidence, not comparable production throughput timings.
"""
import argparse
import contextlib
import hashlib
import http.client
import json
from pathlib import Path
import random
import re
import statistics
import time
from tile_load import batch, digest, fetch, jpeg_dimensions, paths_for, rows, server
from watermark_load import minio_sources, write_mask

TRACE = re.compile(r'S3 io op=(HEAD|GET) source=(\d+) object_bytes=(\d+) offset=(\d+) '
                   r'requested=(\d+) received=(\d+) elapsed_us=(\d+) accepted=(\d+)')


def union_bytes(intervals):
    """Length of the union of half-open intervals; do not double count overlaps."""
    total = 0
    end = 0
    for left, right in sorted(intervals):
        total += max(0, right-max(left,end))
        end = max(end,right)
    return total


def analyze(log, sizes, requests):
    """Group anonymous operations by known fixture length, then by opened source.

    Exact overlap and hypothetical 4 KiB pages are calculated WITHIN each opened
    source, not across unrelated IIIF requests. Page counts are a model, not an
    implemented cache or a claim that every call can safely be coalesced.
    """
    operations=[]
    for match in TRACE.finditer(log):
        op, source, size, offset, wanted, received, elapsed, accepted = match.groups()
        item=dict(op=op,source=int(source),object_bytes=int(size),offset=int(offset),
                  requested=int(wanted),received=int(received),elapsed_us=int(elapsed),accepted=int(accepted))
        if not item['accepted'] or (op=='GET' and item['requested']!=item['received']):
            raise RuntimeError('Failed or incomplete S3 operation in measured workload')
        if item['object_bytes'] not in sizes:
            raise RuntimeError('Unexpected object size in trace')
        item['role']=sizes[item['object_bytes']]
        operations.append(item)
    if sum(o['op']=='HEAD' for o in operations)!=2*requests:
        raise RuntimeError('Missing/extra HEAD events: log boundary or source checks changed')
    groups=[]
    for role in ('image','mask'):
        for op in ('HEAD','GET'):
            selected=[o for o in operations if o['role']==role and o['op']==op]
            times=sorted(o['elapsed_us']/1000 for o in selected)
            wanted=[o['requested'] for o in selected]
            groups.append(dict(role=role,operation=op,count=len(selected),
                               requested_bytes=sum(wanted),received_bytes=sum(o['received'] for o in selected),
                               elapsed_ms=sum(times),p50_ms=statistics.median(times) if times else None,
                               p95_ms=times[(95*len(times)+99)//100-1] if times else None,
                               median_requested_bytes=statistics.median(wanted) if wanted else None,
                               at_most_1k=sum(0<n<=1024 for n in wanted),
                               at_most_4k=sum(0<n<=4096 for n in wanted)))
    overlap=[]
    for role in ('image','mask'):
        selected=[o for o in operations if o['role']==role and o['op']=='GET']
        sources={o['source'] for o in selected}
        unique=0
        page_count=0
        page_bytes=0
        for source in sources:
            reads=[o for o in selected if o['source']==source]
            size=reads[0]['object_bytes']
            unique+=union_bytes([(o['offset'],o['offset']+o['received']) for o in reads])
            pages=set()
            for o in reads:
                pages.update(range(o['offset']//4096,(o['offset']+o['requested']-1)//4096+1))
            page_count+=len(pages)
            page_bytes+=sum(min(4096,size-page*4096) for page in pages)
        overlap.append(dict(role=role,opened_sources_with_get=len(sources),
                            unique_read_bytes=unique,repeated_read_bytes=sum(o['received'] for o in selected)-unique,
                            hypothetical_4k_pages=page_count,hypothetical_4k_bytes=page_bytes))
    return dict(groups=groups,within_source=overlap,operations=operations)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--server',type=Path,required=True)
    parser.add_argument('--images',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--repetitions',type=int,default=3)
    args=parser.parse_args()
    if args.repetitions<1:
        parser.error('Positive repetitions required')
    binary,images,output=(p.resolve() for p in (args.server,args.images,args.output))
    output.mkdir(parents=True,exist_ok=False)
    mask=output/'mask.png'; write_mask(mask)
    policies={}
    for backend,source in [('local',json.dumps(str(mask))),('s3',"{type='s3',key='mask.png'}")]:
        policy=output/(backend+'.lua')
        policy.write_text("function preflight(r)\n"
                          " if r.kind~='image' then return {decision='allow'} end\n"
                          " return {decision='allow',watermark={source="+source+","
                          "width_fraction=0.5,opacity=0.25,color={255,255,255}}}\nend\n")
        policies[backend]=policy
    report=dict(complete=False,utc_started=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),
                server_sha256=digest(binary),runner_sha256=digest(Path(__file__)),clients=8,workers=4,
                repetitions=args.repetitions,mask_sha256=digest(mask),
                sources={n:dict(bytes=(images/n).stat().st_size,sha256=digest(images/n)) for n in ('master.tif','master.jp2')},
                trials=[])

    def save():
        (output/'results.json').write_text(json.dumps(report,indent=2)+'\n')

    save()
    expected={}
    for name in report['sources']:
        expected[name]={}
        with server(binary,images,output/(name+'-reference'),1,False,
                    ['--preflight-script',str(policies['local'])]) as (port,_):
            with contextlib.closing(http.client.HTTPConnection('127.0.0.1',port,timeout=30)) as conn:
                for path in paths_for(name):
                    media,body=fetch(conn,path)
                    if not media.startswith('image/jpeg') or jpeg_dimensions(body)!=(256,256):
                        raise RuntimeError('Invalid reference')
                    expected[name][path]=hashlib.sha256(body).hexdigest()
    rng=random.Random(20261009)
    with minio_sources(images,mask) as remote:
        for repetition in range(1,args.repetitions+1):
            cases=[(name,mode) for name in expected for mode in ('mask-off','mask-on','result-hit')]
            rng.shuffle(cases)
            for name,mode in cases:
                directory=output/f'r{repetition}-{name}-{mode}'
                extra=[*remote,'--preflight-script',str(policies['s3']),
                       '--mask-cache-max-entries',str(0 if mode=='mask-off' else 64)]
                with server(binary,images,directory,4,mode=='result-hit',extra,log_level='debug') as (port,_):
                    with contextlib.closing(http.client.HTTPConnection('127.0.0.1',port,timeout=30)) as conn:
                        fetch(conn,f'/{name}/info.json')
                    paths=paths_for(name)
                    if mode=='result-hit':
                        batch(port,paths,8,expected[name])
                        if rows(directory)[0]!=128: raise RuntimeError('Incomplete warmup')
                    prefix=(directory/'server.log').read_text()
                    measured,_,_=batch(port,paths*(4 if mode=='result-hit' else 1),8,expected[name])
                log=(directory/'server.log').read_text()
                if not log.startswith(prefix): raise RuntimeError('Log changed before measured phase')
                sizes={report['sources'][name]['bytes']:'image',mask.stat().st_size:'mask'}
                stats=analyze(log[len(prefix):],sizes,measured['requests'])
                if mode=='result-hit' and any(o['op']=='GET' for o in stats['operations']):
                    raise RuntimeError('Warm results unexpectedly read an S3 body')
                measured.pop('records')
                report['trials'].append(dict(source=name,mode=mode,repetition=repetition,
                                            http=measured,**stats))
                save()
                print(name,mode,repetition,[(g['role'],g['operation'],g['count']) for g in stats['groups']],flush=True)
    report['complete']=True
    report['minio_cleanup']='Owned temporary objects and bucket deleted'
    save()


if __name__=='__main__':
    main()
