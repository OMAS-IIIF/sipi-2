"""Check exact raster permutations after crop/resize, response sizes and caching."""
from cache_http import server, get, success, rows
from png_check import decode_png
import json
import math
from pathlib import Path
import shutil
import struct
import sys
import tempfile


def transformed_png(original, mirror, turns):
    """Independent oracle: manipulate complete rows/pixels using transpose/reverse."""
    w,h,data,profile,_ = decode_png(original)
    channels=len(data)//(w*h)
    matrix=[[data[(y*w+x)*channels:(y*w+x+1)*channels] for x in range(w)] for y in range(h)]
    if mirror:
        matrix=[row[::-1] for row in matrix]
    for _ in range(turns):
        matrix=[list(row) for row in zip(*matrix[::-1])]
    return len(matrix[0]),len(matrix),b"".join(pixel for row in matrix for pixel in row),profile


def bilinear_reference(original, angle, mirror):
    """Reference using a mirrored source matrix and separable tent kernels."""
    w,h,data,profile,_=decode_png(original)
    channels=len(data)//(w*h)
    gray=channels<=2
    colors=1 if gray else 3
    rows=[[data[(y*w+x)*channels:(y*w+x+1)*channels] for x in range(w)] for y in range(h)]
    if mirror: rows=[row[::-1] for row in rows]
    c,s=math.cos(math.radians(angle)),math.sin(math.radians(angle))
    ow,oh=math.ceil(w*abs(c)+h*abs(s)),math.ceil(w*abs(s)+h*abs(c))
    output=bytearray()
    for y in range(oh):
        for x in range(ow):
            dx,dy=x+.5-ow/2,y+.5-oh/2
            sx,sy=c*dx+s*dy+w/2-.5,-s*dx+c*dy+h/2-.5
            values=[0.]*colors
            alpha=0.
            for yy in (math.floor(sy),math.floor(sy)+1):
                for xx in (math.floor(sx),math.floor(sx)+1):
                    if not (0<=xx<w and 0<=yy<h): continue
                    pixel=rows[yy][xx]
                    weight=(1-abs(sx-xx))*(1-abs(sy-yy))
                    a=weight*(pixel[-1] if channels in (2,4) else 255)
                    alpha+=a
                    for channel in range(colors): values[channel]+=a*pixel[channel]
            quantize=lambda value: max(0,min(255,math.floor(value+.5)))
            a=quantize(alpha)
            output.extend([quantize(v/alpha) if a else 0 for v in values]+[a])
    return ow,oh,bytes(output),profile


def jpeg_size(data):
    """Read the baseline JPEG frame dimensions without decoding lossy samples."""
    assert data[:2]==b'\xff\xd8'
    i=2
    while i<len(data):
        assert data[i]==255
        marker=data[i+1]
        length=int.from_bytes(data[i+2:i+4],'big')
        if marker==0xc0:
            height,width=struct.unpack_from('>HH',data,i+5)
            return width,height
        i+=2+length
    raise AssertionError('JPEG SOF missing')


def main():
    executable,fixtures=(Path(p).resolve() for p in sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix='sipi-rotation-') as temporary:
        root=Path(temporary)
        images=root/'images'; images.mkdir()
        cases=[('tiled.tif','3,5,29,17','7,4'),('gray.tif','3,1,19,11','7,4'),
               ('alpha.tif','1,1,7,5','max'),('alpha-associated.tif','1,1,7,5','3,2'),
               ('pyramid.tif','3,5,65,49','15,12'),('alpha-pyramid.tif','3,1,29,17','14,8'),
               ('source.jp2','1,1,35,21','7,4'),('source.j2k','1,1,35,21','7,4')]
        for name,_,_ in cases:
            shutil.copyfile(fixtures/name,images/name)
        shutil.copyfile(fixtures/'rotated.tif',images/'rotated.tif')
        (root/'policy.lua').write_text("function preflight(request) if request.headers.authorization=='denied' then return {decision='deny'} end return {decision='allow'} end")
        saved=None
        with server(executable,root) as (port,log):
            for name,region,size in cases:
                prefix=f'/{name}/{region}/{size}/'
                base=success(get(port,prefix+'0/default.png'))
                for mirrored in (False,True):
                    for turn in range(4):
                        rotation=('!' if mirrored else '')+str(turn*90)
                        path=prefix+rotation+'/default.png'
                        response=get(port,path)
                        actual=success(response)
                        expected=transformed_png(base,mirrored,turn)
                        assert decode_png(actual)[:4]==expected,(name,rotation)
                        assert success(get(port,path))==actual # A hit must accept post-rotation dimensions.
                        status,headers,body=get(port,path,'HEAD')
                        assert status==200 and not body
                        assert int({k.lower():v for k,v in headers.items()}['content-length'])==len(actual)
                        if name=='tiled.tif':
                            jpeg=success(get(port,prefix+rotation+'/default.jpg'),'image/jpeg')
                            assert jpeg_size(jpeg)==expected[:2]
                            assert success(get(port,prefix+rotation+'/default.jpg'),'image/jpeg')==jpeg
                            if rotation=='!90': saved=(path,actual)
                before=rows(root)
                assert success(get(port,prefix+'360/default.png'))==base
                assert success(get(port,prefix+'!360/default.png'))==success(get(port,prefix+'!0/default.png'))
                assert success(get(port,prefix+'90.0/default.png'))==success(get(port,prefix+'90/default.png'))
                assert rows(root)==before # Angle spellings reuse the existing entries.
                info=json.loads(get(port,f'/{name}/info.json')[2])
                assert set(info['extraFeatures'])=={'mirroring','rotationBy90s','rotationArbitrary'}
                assert 'profile' not in info # Existing explicit development boundary.
            assert get(port,saved[0],authorization='denied')[0]==403
            assert 'extraFeatures' not in json.loads(get(port,'/rotated.tif/info.json')[2])
            for name,region,size in cases:
                prefix=f'/{name}/{region}/{size}/'
                base=success(get(port,prefix+'0/default.png'))
                for angle in (13.,45.,135.,270.125,359.75):
                    for mirror in (False,True):
                        path=prefix+('!' if mirror else '')+str(angle)+'/default.png'
                        actual=success(get(port,path))
                        expected=bilinear_reference(base,angle,mirror)
                        decoded=decode_png(actual)
                        assert decoded[:2]==expected[:2] and decoded[3]==expected[3]
                        assert len(decoded[2])==len(expected[2])
                        assert max(abs(a-b) for a,b in zip(decoded[2],expected[2]))<=1,(name,angle)
                        assert success(get(port,path))==actual
                        assert get(port,path,'HEAD')[0]==200
                        jpeg=success(get(port,path.replace('.png','.jpg')),'image/jpeg')
                        assert jpeg_size(jpeg)==expected[:2]
                        if name=='tiled.tif' and angle==45 and mirror: saved=(path,actual)
            assert get(port,saved[0],authorization='denied')[0]==403
            # All transformed PNG results have matching actual encoded/index dimensions.
            for digest,length,media,w,h in rows(root):
                if media==b'image/png':
                    name=digest.decode()
                    data=(root/'cache'/'objects'/name[:2]/(name+'.bin')).read_bytes()
                    assert len(data)==length and decode_png(data)[:2]==(w,h)
        assert 'lookup failed' not in log[0] and 'publication failed' not in log[0],log[0]
        with server(executable,root) as (port,log):
            assert success(get(port,saved[0]))==saved[1]
        assert 'IIIF result cache hit' in log[0] and 'IIIF result cache miss' not in log[0]
    print('HTTP mirroring/quarter turns, exact PNG pixels, JPEG/HEAD/cache dimensions passed')


if __name__=='__main__':
    main()
