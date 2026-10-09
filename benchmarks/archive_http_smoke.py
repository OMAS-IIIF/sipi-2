#!/usr/bin/env python3
"""Verify prepared pairs through SIPI HTTP, without measuring performance.

Own disposable loopback servers, one root per prepared pair. Check information,
exact unscaled PNG crops (including bottom/right edges), and reduced full views.
Reduced TIFF/JP2 pixels need not be equal: their resolution filters differ.
"""
import argparse
import contextlib
import http.client
import io
import json
from pathlib import Path

from PIL import Image
from tile_load import server, fetch, digest

Image.MAX_IMAGE_PIXELS = 300_000_000


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--images', required=True, type=Path)
    parser.add_argument('--server', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    options = parser.parse_args()
    images, binary, output = (p.resolve() for p in (options.images,options.server,options.output))
    manifest = json.loads((images/'manifest.json').read_text())
    if not manifest['complete']:
        raise ValueError('Preparation must be complete')
    output.mkdir(parents=True, exist_ok=False)
    report = {'complete':False, 'manifest_sha256':digest(images/'manifest.json'),
              'server_sha256':digest(binary),
              'output_policy':{'max_width':2048,'max_height':2048,'max_area':4194304}, 'checks':[]}
    def save():
        (output/'results.json').write_text(json.dumps(report,indent=2)+'\n')
    save()
    for name, prepared in manifest['prepared'].items():
        width,height = prepared['size']
        with Image.open(images/name/'reference.tif') as source:
            golden = [source.crop((11,7,268,200)), source.crop((width-97,height-83,width,height))]
        with server(binary,images/name,output/name,2,False,
                    extra=('--max-width','2048','--max-height','2048','--max-area','4194304')) as (port,_):
            with contextlib.closing(http.client.HTTPConnection('127.0.0.1',port,timeout=60)) as conn:
                for filename in ('pyramid.tif','image.jp2'):
                    _, body = fetch(conn,f'/{filename}/info.json')
                    info = json.loads(body)
                    if (info['width'],info['height']) != (width,height):
                        raise ValueError('HTTP metadata dimensions differ')
                    report['checks'].append({'image':name,'file':filename,'request':'info.json','ok':True})
                    regions = ['11,7,257,193',f'{width-97},{height-83},97,83','full']
                    for index,region in enumerate(regions):
                        path=f'/{filename}/{region}/{"256," if region=="full" else "max"}/0/default.png'
                        media,body=fetch(conn,path)
                        with Image.open(io.BytesIO(body)) as decoded:
                            decoded.load()
                            if media.split(';')[0] != 'image/png' or decoded.format != 'PNG':
                                raise ValueError('Wrong HTTP image type')
                            if index<2:
                                if decoded.mode != golden[index].mode or decoded.size != golden[index].size or decoded.tobytes()!=golden[index].tobytes():
                                    raise ValueError(f'HTTP native crop differs: {name} {path}')
                            elif decoded.width!=256 or abs(decoded.height-height*256/width)>1:
                                raise ValueError('Wrong reduced dimensions')
                            report['checks'].append({'image':name,'file':filename,'request':path,'size':list(decoded.size),'ok':True})
                    save()
        print(f'{name}: TIFF/JP2 metadata, exact native crops and reduced views passed',flush=True)
    report['complete']=True
    save()


if __name__=='__main__':
    main()
