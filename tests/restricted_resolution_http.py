"""Public virtual canvas: pixel oracles, access/cache isolation and source versions."""
from pathlib import Path
import concurrent.futures
import json
import shutil
import sys
import tempfile
from cache_http import server, get, success
from png_metadata import fixture, expected_pixels
from png_check import decode_png
from watermark_preflight_http import mask


def crop(pixels, width, channels, x, y, w, h):
    return b''.join(pixels[((y+j)*width+x)*channels:((y+j)*width+x+w)*channels] for j in range(h))


def main():
    executable, fixtures, jpeg_fixtures = (Path(p).resolve() for p in sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix='sipi-restricted-') as temporary:
        root = Path(temporary)
        images = root/'images'
        images.mkdir()
        (images/'image.png').write_bytes(fixture(width=6,height=4))
        (images/'mask.png').write_bytes(mask(255))
        shutil.copyfile(jpeg_fixtures/'rgb.jpg',images/'rgb.jpg')
        for name in ('tiled.tif','watermark-pyramid.tif','source.jp2','offset.jp2','alpha.tif','gray.tif'):
            shutil.copyfile(fixtures/name,images/name)
        (root/'policy.lua').write_text('''function preflight(r)
          local mode=r.prefix[1]
          if r.headers.authorization=='deny' then return {decision='deny'} end
          if mode=='original' and r.headers.authorization=='owner' then return {decision='allow'} end
          if mode~='public' and mode~='small' and mode~='marked' then return {decision='deny'} end
          local limit=(r.identifier=='image.png' or r.identifier=='alias') and 3 or 20
          if mode=='small' then limit=1 end
          local result={decision='allow',resolution={max_width=limit,max_height=limit}}
          if r.identifier=='alias' then result.source='image.png' end
          if mode=='marked' then result.watermark={source='mask.png',width_fraction=0.5,opacity=0.5,color={255,255,255}} end
          return result
        end''')
        url='/public/image.png/full/max/0/default.png'
        # Independent exact 2x2 area oracle: no production resampler as reference.
        original=expected_pixels(width=6,height=4)
        expected=bytes((sum(original[((2*y+dy)*6+2*x+dx)*3+c] for dy in range(2) for dx in range(2))+2)//4
                       for y in range(2) for x in range(3) for c in range(3))
        with server(executable,root,extra=('--size-upscaling=true','--iiif-workers=4')) as (port,log):
            data=success(get(port,url)); assert decode_png(data)[:3]==(3,2,expected)
            assert '/public/image.png/' in get(port,url)[1]['link']
            alias=get(port,'/public/alias/full/max/0/default.png')
            assert alias[0]==200 and alias[2]==data and '/public/alias/' in alias[1]['link']
            info=json.loads(success(get(port,'/public/image.png/info.json'),mime='application/ld+json'))
            assert (info['width'],info['height'])==(3,2) and info['id'].endswith('/public/image.png')
            assert get(port,'/public/image.png')[1]['location'].endswith('/public/image.png/info.json')
            assert get(port,url,'HEAD')[2]==b''
            assert decode_png(success(get(port,'/public/image.png/1,0,2,2/max/0/default.png')))[:3]==(2,2,crop(expected,3,3,1,0,2,2))
            assert get(port,'/public/image.png/3,0,1,1/max/0/default.png')[0]==400
            # Upscaling repeats only public detail, even at original output dimensions.
            enlarged=decode_png(success(get(port,'/public/image.png/1,0,1,1/^6,4/0/default.png')))
            assert enlarged[:3]==(6,4,expected[3:6]*24)
            assert decode_png(success(get(port,'/public/image.png/full/max/!90/default.png')))[:2]==(2,3)
            for ext in ('jpg','webp'):
                assert get(port,f'/public/image.png/full/max/0/default.{ext}')[0]==200
            for quality in ('gray','bitonal'):
                assert get(port,f'/public/image.png/full/max/0/{quality}.png')[0]==200
            full=success(get(port,'/original/image.png/full/max/0/default.png',authorization='owner'))
            assert decode_png(full)[:3]==(6,4,original)
            for path in ('/original/image.png/info.json','/original/image.png','/original/image.png/full/max/0/default.png',url):
                assert get(port,path,authorization='deny')[0]==403
            assert get(port,'/original/image.png/full/max/0/default.png')[0]==403
            assert get(port,'/image.png/full/max/0/default.png')[0]==403
            assert decode_png(success(get(port,'/small/image.png/full/max/0/default.png')))[:2]==(1,1)
            # Different crops share the same reduced source (not independent decoder ROIs).
            with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
                def tile(x):
                    result=decode_png(success(get(port,f'/public/image.png/{x},0,1,2/max/0/default.png')))
                    assert result[:3]==(1,2,crop(expected,3,3,x,0,1,2))
                list(pool.map(tile,[0,1,2]*4))
            # Native decoder adapters, alpha/gray and full-canvas watermark consistency.
            for name in ('tiled.tif','watermark-pyramid.tif','source.jp2','offset.jp2','alpha.tif','gray.tif','rgb.jpg'):
                for prefix in (('public',) if name in ('alpha.tif','gray.tif') else ('public','marked')):
                    base=f'/{prefix}/{name}'
                    w,h,pixels,*_=decode_png(success(get(port,base+'/full/max/0/default.png')))
                    assert w<=20 and h<=20
                    channels=len(pixels)//(w*h)
                    x,y=1,1; cw,ch=max(1,w-2),max(1,h-2)
                    part=decode_png(success(get(port,f'{base}/{x},{y},{cw},{ch}/max/0/default.png')))
                    assert part[:3]==(cw,ch,crop(pixels,w,channels,x,y,cw,ch)),name
            # Source replacement invalidates both resident and persistent results.
            (images/'replacement.png').write_bytes(fixture(width=8,height=4))
            (images/'replacement.png').replace(images/'image.png')
            replaced=success(get(port,url)); assert replaced!=data
            assert decode_png(replaced)[:2]==(3,2)
            (images/'image.png').unlink()
            assert get(port,url)[0]==404
        assert 'IIIF variant cache hit' in log[0] and 'IIIF result cache hit' in log[0]
        (images/'image.png').write_bytes(fixture(width=6,height=4))
        # Restart, disabled derivative retention, disabled persistent storage.
        for extra,cache in [(('--variant-cache-max-entries=0',),'cache'),
                            (('--variant-cache-max-bytes=0',),None),
                            (('--max-width=2','--max-height=2'),None)]:
            with server(executable,root,extra=extra,cache=cache) as (port,log):
                info=json.loads(get(port,'/public/image.png/info.json')[2])
                assert (info['width'],info['height'])==(3,2)
                result=success(get(port,url))
                if not extra[0].startswith('--max-width'):
                    assert decode_png(result)[:3]==(3,2,expected)
                else:
                    assert decode_png(result)[:2]==(2,1)
            if cache is None and extra[0].startswith('--variant'):
                assert 'variant cache admission declined' in log[0]
        # Environment wiring and CLI precedence in a real server process.
        with server(executable,root,cache=None,variables={'SIPI_VARIANT_CACHE_MAX_BYTES':'0'}) as (port,log):
            assert decode_png(success(get(port,url)))[:3]==(3,2,expected)
        assert 'variant cache admission declined' in log[0]
        with server(executable,root,cache=None,extra=('--variant-cache-max-bytes=67108864',),
                    variables={'SIPI_VARIANT_CACHE_MAX_BYTES':'0'}) as (port,log):
            success(get(port,url)); success(get(port,'/public/image.png/1,0,1,1/max/0/default.png'))
        assert 'variant cache hit' in log[0] and 'variant cache admission declined' not in log[0]
        with server(executable,root) as (port,log):
            assert decode_png(success(get(port,url)))[:3]==(3,2,expected)
        assert 'IIIF result cache hit' in log[0] and 'IIIF variant cache miss' not in log[0]
        # Invalid restriction must not fall through to the original source/cache.
        (root/'policy.lua').write_text("function preflight() return {decision='allow',resolution={max_width=0,max_height=1000}} end")
        with server(executable,root) as (port,_):
            for path in (url,'/public/image.png/info.json','/public/image.png'):
                result=get(port,path); assert result[0]==500 and json.loads(result[2])['error']=='preflight_failed'
    print('Restricted public resolution HTTP checks passed')


if __name__=='__main__':
    main()
