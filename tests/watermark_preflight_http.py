"""Real delivery, policy separation, mask version changes and fail-closed reuse."""
from pathlib import Path
import concurrent.futures
import json
import shutil
import struct
import sys
import tempfile
import zlib
from cache_http import server, get
from png_metadata import fixture, chunk, expected_pixels
from png_check import decode_png


def mask(value):
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 1, 1, 8, 0, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(bytes([0, value]))) + chunk(b'IEND', b''))


def main():
    with tempfile.TemporaryDirectory(prefix='sipi-watermark-policy-') as temporary:
        root = Path(temporary)
        images = root / 'images'
        images.mkdir()
        (images / 'image.png').write_bytes(fixture(width=6, height=4))
        (images / 'mask.png').write_bytes(mask(255))
        for name in ('tiled.tif','pyramid.tif','watermark-pyramid.tif','source.jp2','offset.jp2'):
            shutil.copyfile(Path(sys.argv[2])/name, images/name)
        (root / 'policy.lua').write_text('''function preflight(r)
          local auth=r.headers.authorization
          if auth == 'plain' then return {decision='allow'} end
          if auth == 'deny' then return {decision='deny'} end
          local mark={source='mask.png',width_fraction=0.5,opacity=0.5,color={255,255,255}}
          if auth == 'bad' then mark.opacity=2 end
          if auth == 'opaque' then mark.opacity=1 end
          if auth == 'wide' then mark.width_fraction=1 end
          if auth == 'red' then mark.color={255,0,0} end
          if auth == 'missing' then mark.source='private-missing-mask.png' end
          return {decision='allow',watermark=mark}
        end''')
        executable = Path(sys.argv[1]).resolve()
        image = '/image.png/full/max/0/default.png'
        with server(executable, root) as (port, log):
            plain = get(port, image, authorization='plain')[2]
            assert decode_png(plain)[2] == expected_pixels(width=6,height=4)
            status, headers, marked = get(port, image, authorization='marked')
            assert status == 200 and headers['cache-control'] == 'no-store'
            expected = bytearray(decode_png(plain)[2])
            # 3x3 centered mark on 6x4 canvas: x=1, y=0, white at half opacity.
            for y in range(3):
                for x in range(1,4):
                    for c in range(3):
                        i=(y*6+x)*3+c
                        expected[i]=(expected[i]+256)//2
            assert decode_png(marked)[2] == expected
            assert get(port,image,authorization='marked')[2] == marked
            assert get(port,image,authorization='plain')[2] == plain
            variants = [get(port,image,authorization=a)[2] for a in ('opaque','wide','red')]
            assert len(set([plain,marked,*variants])) == 5
            for _ in range(2):
                for a, data in zip(('opaque','wide','red'),variants):
                    assert get(port,image,authorization=a)[2] == data
            crop = get(port,'/image.png/1,1,3,2/max/0/default.png',authorization='marked')
            assert crop[0] == 200
            assert decode_png(crop[2])[2] == b''.join(expected[(y*6+1)*3:(y*6+4)*3] for y in (1,2))
            rotated = get(port,'/image.png/full/max/!90/default.png',authorization='marked')
            assert rotated[0] == 200 and decode_png(rotated[2])[:2] == (4,6)
            for fmt in ('jpg','webp'):
                url=f'/image.png/full/max/0/default.{fmt}'
                assert get(port,url,authorization='marked')[0] == 200
                assert get(port,url,authorization='marked')[2] != get(port,url,authorization='plain')[2]
            for quality in ('gray','bitonal'):
                assert get(port,f'/image.png/full/max/0/{quality}.png',authorization='marked')[0] == 200
            for method in ('GET','HEAD'):
                status, _, body=get(port,'/image.png/info.json',method,'marked')
                assert status==200
                if method=='GET':
                    info=json.loads(body); assert (info['width'],info['height'])==(6,4)
                else: assert not body
                assert get(port,'/image.png',method,'marked')[0]==303
                assert get(port,image,method,'marked')[0]==200
            with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
                results=list(pool.map(lambda a:get(port,image,authorization=a)[2], ['plain','marked']*6))
            assert results == [plain,marked]*6
            for auth, code in [('bad','preflight_failed'),('missing','watermark_failed'),('deny','access_denied')]:
                for path in (image,'/image.png/info.json','/image.png'):
                    status, _, body=get(port,path,authorization=auth)
                    assert status==(403 if auth=='deny' else 500)
                    assert json.loads(body)['error']==code and b'private-' not in body
            # Opaque preservation-only ICC headers cannot transform a watermark color.
            assert get(port,'/pyramid.tif/full/26,/0/default.png',authorization='marked')[0]==500
            # Real decoder adapters: native tile windows and reduced TIFF/J2K grids.
            # Opaque constant coverage gives a white center independent of source colors.
            for name, size in [('tiled.tif','max'),('watermark-pyramid.tif','26,'),
                               ('source.jp2','max'),('source.jp2','10,'),('offset.jp2','10,')]:
                path=f'/{name}/full/{size}/0/default.png'
                status, _, data=get(port,path,authorization='opaque')
                assert status==200, (path,status,data)
                w,h,pixels,*_=decode_png(data)
                assert pixels[((h//2)*w+w//2)*3:((h//2)*w+w//2)*3+3]==bytes([255]*3), path
                assert data!=get(port,path,authorization='plain')[2], path
                assert data==get(port,path,authorization='opaque')[2], path
            for name in ('tiled.tif','source.jp2','offset.jp2'):
                full=decode_png(get(port,f'/{name}/full/max/0/default.png',authorization='marked')[2])
                crop=get(port,f'/{name}/3,2,20,15/max/0/default.png',authorization='marked')
                assert crop[0]==200
                w,_,pixels,*_=full
                assert decode_png(crop[2])[2]==b''.join(pixels[(y*w+3)*3:(y*w+23)*3] for y in range(2,17)), name
            # Atomic replacement changes source version even when encoded length matches.
            (images/'new-mask.png').write_bytes(mask(0)); (images/'new-mask.png').replace(images/'mask.png')
            assert get(port,image,authorization='marked')[2] == plain
            (images/'new-mask.png').write_bytes(b'not a png'); (images/'new-mask.png').replace(images/'mask.png')
            assert get(port,image,authorization='marked')[0] == 500
            (images/'mask.png').unlink()
            assert get(port,image,authorization='marked')[0] == 500
            assert get(port,image,authorization='plain')[2] == plain
        assert 'IIIF result cache hit' in log[0]
        assert 'IIIF mask cache hit' in log[0]
        assert 'IIIF mask cache miss' in log[0]
        # Persistent result must still require the current policy and mask after restart.
        with server(executable,root) as (port,_):
            assert get(port,image,authorization='marked')[0] == 500
            assert get(port,image,authorization='plain')[2] == plain
        # No persistent result cache: different renders still reuse one decoded mask.
        (images/'mask.png').write_bytes(mask(255))
        with server(executable,root,cache=None) as (port,log):
            assert get(port,image,authorization='marked')[0]==200
            assert get(port,'/image.png/full/3,/0/default.png',authorization='red')[0]==200
        assert log[0].count('IIIF mask cache miss')==1
        assert log[0].count('IIIF mask cache hit')==1
        # Environment is wired through the executable; either zero disables retention.
        with server(executable,root,cache=None,
                    variables={'SIPI_MASK_CACHE_MAX_ENTRIES':'0'}) as (port,log):
            first=get(port,image,authorization='marked')
            assert first[0]==200 and get(port,image,authorization='marked')[2]==first[2]
        assert 'IIIF mask cache hit' not in log[0]
        assert log[0].count('IIIF mask cache miss')==2
        assert log[0].count('IIIF mask cache admission declined')==2
    print('Watermark pixels, variants/cache, source replacement and failures passed')


if __name__ == '__main__':
    main()
