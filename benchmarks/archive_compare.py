#!/usr/bin/env python3
"""Balanced real-corpus comparison: TIFF/JP2, native and nonaligned requests.

Shares Docker ownership and timing with server_compare.py; never touches OLDAP.
Run --check-only before timing to inspect decoder, geometry and colour differences.
Each client completes whole six-image rounds, avoiding a fast-image traffic bias.
"""
import argparse
import contextlib
import hashlib
import http.client
import io
import json
import math
import os
from pathlib import Path
import platform
import random
import statistics
import time

from PIL import Image, ImageChops, ImageCms, ImageStat
from server_compare import server, batch, docker
from tile_load import digest, fetch

Image.MAX_IMAGE_PIXELS = 300_000_000
SRGB = ImageCms.createProfile('sRGB')


def requests_for(stem, width, height, source_format, output_format):
    """Five requests per image, all output axes <=2048; unaligned even at 1:1."""
    if min(width,height) < 1100:
        raise ValueError('This corpus workload requires both source axes >=1100')
    regions = [
        ('native-offset',(17,29,256,256),'256,256',(256,256)),
        ('native-edge',(width-256,height-256,256,256),'256,256',(256,256)),
        ('fractional-square',((width-513)//2,(height-513)//2,513,513),'256,256',(256,256)),
        ('fractional-rectangle',((width-1023)//2,(height-769)//2,1023,769),'384,288',(384,288)),
        ('overview',(0,0,width,height),'512,',(512,round(height*512/width))),
    ]
    result=[]
    for category,region,size,dimensions in regions:
        token='full' if category=='overview' else ','.join(map(str,region))
        result.append({'stem':stem,'category':category,'region':region,'size':dimensions,
                       'path':f'/{stem}.{source_format}/{token}/{size}/0/default.{output_format}',
                       'native':category.startswith('native-')})
    return result


def display_rgb(image, fallback_profile=None):
    """Diagnostic common sRGB view; preserve raw-pixel differences separately.

    Honour embedded ICC. Untagged RGB is assumed sRGB; untagged L is assigned the
    normalized source's Gray Gamma 2.2 profile, an explicit diagnostic assumption.
    """
    profile=image.info.get('icc_profile')
    if not profile and image.mode=='L':
        profile=fallback_profile
    if profile:
        return ImageCms.profileToProfile(image,ImageCms.ImageCmsProfile(io.BytesIO(profile)),SRGB,outputMode='RGB')
    return image.convert('RGB')


def references(port,prefix,corpus,manifest,directory):
    """Decode every output and audit native crops/ICC; don't equate resamplers."""
    expected,records={},[]
    with contextlib.closing(http.client.HTTPConnection('127.0.0.1',port,timeout=120)) as conn:
        for stem,prepared in manifest['prepared'].items():
            with Image.open(corpus/stem/'reference.tif') as source:
                profile=source.info['icc_profile']
                # Cache only small golden crops, not six full source rasters.
                golden={}
                for request in requests_for(stem,*prepared['size'],'tif','png'):
                    x,y,w,h=request['region']
                    crop=source.crop((x,y,x+w,y+h))
                    if not request['native']:
                        crop=crop.resize(request['size'],Image.Resampling.BOX)
                    crop.info['icc_profile']=profile
                    golden[request['category']]=crop
            for source_format in ('tif','jp2'):
                _,body=fetch(conn,f'{prefix}/{stem}.{source_format}/info.json')
                info=json.loads(body)
                if (info['width'],info['height'])!=tuple(prepared['size']):
                    raise ValueError(f'Wrong info dimensions: {stem}.{source_format}')
                for output_format in ('jpg','png'):
                    for request in requests_for(stem,*prepared['size'],source_format,output_format):
                        path=prefix+request['path']
                        media,body=fetch(conn,path)
                        if media.split(';')[0]!={'jpg':'image/jpeg','png':'image/png'}[output_format]:
                            raise ValueError(f'Wrong MIME: {path}')
                        with Image.open(io.BytesIO(body)) as decoded:
                            decoded.load()
                            wanted=request['size']
                            if decoded.width!=wanted[0] or abs(decoded.height-wanted[1])>(1 if request['category']=='overview' else 0):
                                raise ValueError(f'Wrong dimensions {decoded.size}: {path}')
                            reference=golden[request['category']]
                            if decoded.size!=reference.size:
                                reference=reference.resize(decoded.size,Image.Resampling.BOX)
                                reference.info['icc_profile']=profile
                            raw=ImageStat.Stat(ImageChops.difference(reference.convert('RGB'),decoded.convert('RGB'))).mean
                            colour=ImageStat.Stat(ImageChops.difference(display_rgb(reference),display_rgb(decoded,profile))).mean
                            if request['native'] and output_format=='png' and any(colour):
                                raise ValueError(f'Native PNG differs in common sRGB: {path}, MAE={colour}')
                            record=request | {'source_format':source_format,'output_format':output_format,
                                'path':path,'bytes':len(body),'decoded_mode':decoded.mode,
                                'icc_sha256':hashlib.sha256(decoded.info.get('icc_profile',b'')).hexdigest(),
                                'raw_mae_rgb':raw,'srgb_mae_rgb':colour,
                                'sampling':getattr(decoded,'layer',None),
                                'png_native_exact': all(v==0 for v in raw) if request['native'] and output_format=='png' else None}
                            records.append(record)
                        expected[path]=(media.split(';')[0],hashlib.sha256(body).hexdigest())
            print(f'  references: {stem}',flush=True)
    (directory/'references.json').write_text(json.dumps({'records':records,'hashes':expected},indent=2)+'\n')
    return expected,records


def summary(report):
    """Compact per-trial and grouped results, retaining image/category latencies."""
    result={k:v for k,v in report.items() if k!='trials'}
    result['trials']=[{k:v for k,v in t.items() if k!='records_ms_bytes'} for t in report['trials']]
    groups={}
    for trial in result['trials']:
        key=(trial['server'],trial['cached'],trial['source_format'],trial['output_format'],trial['clients'])
        groups.setdefault(key,[]).append(trial)
    result['groups']=[]
    for key,trials in sorted(groups.items()):
        row=dict(zip(('server','cached','source_format','output_format','clients'),key))
        row['repetitions']=len(trials)
        for metric in ('rps','p50_ms','p95_ms'):
            values=[t[metric] for t in trials]
            row[metric]={'median':statistics.median(values),'min':min(values),'max':max(values)}
        result['groups'].append(row)
    return result


def jpeg_variants(baseline, turbo):
    """Controlled server-level A/B: retain old defaults, vary new sampling only.

    The historical image has no JPEG flags and hardcodes quality 90 / 4:4:4.
    Labels keep byte stability checks and timing groups separate per variant.
    """
    return {
        'ijg-444': {'kind': 'sipi', 'image': baseline, 'jpeg_options': None, 'sampling': '4:4:4'},
        'turbo-444': {'kind': 'sipi', 'image': turbo, 'jpeg_options': (90, '4:4:4'), 'sampling': '4:4:4'},
        'turbo-420': {'kind': 'sipi', 'image': turbo, 'jpeg_options': (90, '4:2:0'), 'sampling': '4:2:0'},
    }


def check_sampling(records, sampling):
    """Assert actual JPEG SOF factors; grayscale always has one component."""
    wanted = [(1, 1)] if sampling == '4:4:4' else [(2, 2), (1, 1), (1, 1)]
    for record in records:
        if record['output_format'] != 'jpg':
            continue
        actual = [(component[1], component[2]) for component in record['sampling']]
        expected = [(1, 1)] if record['decoded_mode'] == 'L' else (wanted * 3 if sampling == '4:4:4' else wanted)
        if actual != expected:
            raise ValueError(f"Unexpected JPEG sampling for {record['path']}: {actual}")


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--corpus',type=Path,required=True)
    parser.add_argument('--sample',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--servers',nargs='+',choices=['sipi','cantaloupe','iip','triplet'],default=['sipi','cantaloupe','iip','triplet'])
    parser.add_argument('--jpeg-comparison',action='store_true',
                        help='Compare historical IJG 90/444 with turbo 90/444 and 90/420; replaces --servers')
    parser.add_argument('--baseline-image',default='lrosenth/sipi-2:verify-0.1.0-arm64')
    parser.add_argument('--turbo-image',default='sipi-benchmark/sipi-2-turbo:verify-0.1.0-arm64')
    parser.add_argument('--check-only',action='store_true')
    parser.add_argument('--repetitions',type=int,default=3)
    parser.add_argument('--minimum-seconds',type=float,default=5)
    parser.add_argument('--warmup-seconds',type=float,default=15)
    options=parser.parse_args()
    if min(options.repetitions,options.minimum_seconds,options.warmup_seconds)<=0:
        parser.error('Counts and times must be positive')
    corpus,sample,output=(p.resolve() for p in (options.corpus,options.sample,options.output))
    manifest=json.loads((corpus/'manifest.json').read_text())
    if not manifest['complete']:
        raise ValueError('Incomplete corpus')
    output.mkdir(parents=True,exist_ok=False)
    sources=output/'sources';sources.mkdir()
    for stem,prepared in manifest['prepared'].items():
        for ext,filename in [('tif','pyramid.tif'),('jp2','image.jp2')]:
            path=corpus/stem/filename
            if digest(path)!=prepared['files'][filename]['sha256']:
                raise ValueError('Prepared corpus changed')
            os.link(path,sources/f'{stem}.{ext}')
    tags={'sipi':'lrosenth/sipi-2:verify-0.1.0-arm64',
          'cantaloupe':'sipi-benchmark/cantaloupe-openjpeg:5.0.7',
          'iip':'iipsrv/iipsrv@sha256:11cedb79da309ef0b766c2fadddb5eab6b9bb3a4eb24b49ab941a431d78c9a9c',
          'triplet':'ghcr.io/libops/triplet@sha256:3dd347c3f7e9021c7c32b995f2417a294c881436c497f6b264496f3d8ea65dbf'}
    variants = {name: {'kind': name, 'image': tags[name], 'jpeg_options': None} for name in options.servers}
    if options.jpeg_comparison:
        variants = jpeg_variants(options.baseline_image, options.turbo_image)
    if docker('info','--format','{{.Architecture}}') not in ('arm64','aarch64'):
        raise ValueError('Native ARM64 engine required')
    pinned={}
    for name,variant in variants.items():
        image=json.loads(docker('image','inspect',variant['image']))[0]
        if image['Architecture']!='arm64':raise ValueError('Native ARM64 image required')
        pinned[name]=image['Id']
    report={'complete':False,'variants':variants,'images':pinned,'host':platform.platform(),
            'docker':json.loads(docker('info','--format','{{json .}}')),
            'options':vars(options)|{'corpus':str(corpus),'sample':str(sample),'output':str(output)},
            'corpus_sha256':digest(corpus/'manifest.json'),'sample_sha256':digest(sample),
            'runner_sha256':digest(Path(__file__)),'shared_runner_sha256':digest(Path(__file__).with_name('server_compare.py')),
            'started_utc':time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),'trials':[],'references':{}}
    report['docker']={key:report['docker'][key] for key in ('ServerVersion','Architecture','NCPU','MemTotal','KernelVersion')}
    reference_hashes={}
    def save():
        (output/'results.json').write_text(json.dumps(report,indent=2)+'\n')
        (output/'summary.json').write_text(json.dumps(summary(report),indent=2)+'\n')
    save()
    for filename in ('archive_compare.py','server_compare.py','tile_load.py'):
        (output/filename).write_bytes(Path(__file__).with_name(filename).read_bytes())
    first=next(iter(manifest['prepared']))
    rng=random.Random(20261009)
    for repetition in range(1 if options.check_only else options.repetitions):
        cases=[(name,cached) for name in pinned for cached in ([False] if options.check_only else [False,True])]
        rng.shuffle(cases)
        for name,cached in cases:
            directory=output/f'{repetition}-{name}-{"hits" if cached else "render"}'
            print(f'Starting {directory.name}',flush=True)
            variant = variants[name]
            with server(variant['kind'],pinned[name],sources,sample,directory,cached,
                        readiness=(first+'.tif',*manifest['prepared'][first]['size']),archive=True,
                        jpeg_options=variant['jpeg_options']) as (port,prefix,_):
                expected,records=references(port,prefix,corpus,manifest,directory)
                if 'sampling' in variant:
                    check_sampling(records, variant['sampling'])
                previous=reference_hashes.setdefault(name,expected)
                if previous!=expected:
                    raise ValueError(f'Response bytes differ across repetitions/cache modes: {name}')
                report['references'][directory.name]=records
                save()
                if options.check_only:continue
                until=time.perf_counter()+options.warmup_seconds
                while time.perf_counter()<until:
                    batch(port,list(expected),8,expected,balanced=True)
                workloads=[(src,out,clients) for src in ('tif','jp2') for out in ('jpg','png') for clients in (1,8)]
                rng.shuffle(workloads)
                metadata={r['path']:r for r in records}
                for src,out,clients in workloads:
                    paths=[r['path'] for r in records if r['source_format']==src and r['output_format']==out]
                    rng.shuffle(paths)
                    result=batch(port,paths,clients,expected,options.minimum_seconds,balanced=True,record_paths=True)
                    result.update(server=name,cached=cached,source_format=src,output_format=out,clients=clients,repetition=repetition)
                    # Per-image/category latency diagnostics, not standalone throughput claims.
                    details={}
                    for latency,size,path in result['records_ms_bytes']:
                        m=metadata[path];key=m['stem']+'/'+m['category']
                        details.setdefault(key,[]).append(latency)
                    result['by_request']={key:{'count':len(v),'p50_ms':statistics.median(v),
                                              'p95_ms':sorted(v)[math.ceil(.95*len(v))-1]} for key,v in details.items()}
                    report['trials'].append(result);save()
                    print(f'  {src}->{out} c={clients}: {result["rps"]:.1f} req/s, p95={result["p95_ms"]:.1f}ms',flush=True)
    # Hardlinks retain the prepared files, so recheck the corpus after all readers exit.
    for stem,prepared in manifest['prepared'].items():
        for filename in ('pyramid.tif','image.jp2'):
            if digest(corpus/stem/filename)!=prepared['files'][filename]['sha256']:raise ValueError('Corpus changed during run')
    report['complete']=True
    report['finished_utc']=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime())
    save()


if __name__=='__main__':
    main()
