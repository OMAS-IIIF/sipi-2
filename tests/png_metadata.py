"""Generate tiny independent PNG fixtures, then run C++ header-reader checks."""
from pathlib import Path
import struct
import subprocess
import sys
import zlib


def chunk(name, data):
    return struct.pack('>I', len(data)) + name + data + struct.pack('>I', zlib.crc32(name + data))


def samples(color, bits, x, y):
    gray = (x*53+y*79) % 256 if bits >= 8 else (x+y) % (1 << bits)
    rgb = [(x*53+y*79+c*41) % 256 for c in range(3)] if x or y else [0,0,0]
    alpha = [0,91,255][(x+y)%3]
    return {0:[gray], 2:rgb, 3:[(x+y)%2], 4:[gray,alpha], 6:rgb+[alpha]}[color]


def expected_pixels(color=2, bits=8, transparency=False, width=3, height=2):
    output = bytearray()
    for y in range(height):
        for x in range(width):
            values = samples(color,bits,x,y)
            if color == 3:
                values = [255*values[0]]*3 + ([255*values[0]] if transparency else [])
            elif color == 0:
                value = values[0]*255//((1<<bits)-1)
                values = [value,0 if value == 0 else 255] if transparency else [value]
            elif color == 4:
                values = [values[0],values[1]]
            elif color == 2 and transparency:
                values += [0 if values == [0,0,0] else 255]
            output.extend(values)
    return bytes(output)


def fixture(color=2, bits=8, extra=b'', interlace=0, width=3, height=2):
    header = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, bits, color, 0, 0, interlace))
    # Independently construct all seven Adam7 passes with filter type zero.
    passes = [(0,0,1,1)] if not interlace else [(0,0,8,8),(4,0,8,8),(0,4,4,8),
              (2,0,4,4),(0,2,2,4),(1,0,2,2),(0,1,1,2)]
    data = bytearray()
    for x0,y0,dx,dy in passes:
        if x0 >= width or y0 >= height:
            continue
        for y in range(y0,height,dy):
            values = [v for x in range(x0,width,dx) for v in samples(color,bits,x,y)]
            data.append(0)
            if bits < 8:
                for start in range(0,len(values),8//bits):
                    value = 0
                    for i,v in enumerate(values[start:start+8//bits]):
                        value |= v << (8-bits*(i+1))
                    data.append(value)
            elif bits == 16:
                for value in values:
                    data.extend(struct.pack('>H',value*257))
            else:
                data.extend(values)
    return header + extra + chunk(b'IDAT',zlib.compress(data)) + chunk(b'IEND',b'')


def main():
    executable, directory = map(Path, sys.argv[1:])
    directory.mkdir(parents=True, exist_ok=True)
    profile = bytearray(132)
    for offset, value in {0:132, 8:0x04000000, 12:0x6d6e7472, 16:0x52474220,
                          20:0x58595a20, 36:0x61637370, 68:0xf6d6, 72:0x10000, 76:0xd32d}.items():
        struct.pack_into('>I', profile, offset, value)
    (directory/'expected.icc').write_bytes(profile)
    palette = chunk(b'PLTE', b'\0\0\0\xff\xff\xff')
    rgb = fixture()
    fixtures = {
        'rgb':rgb, 'gray':fixture(0), 'rgba':fixture(6), 'gray-alpha':fixture(4),
        'palette':fixture(3,2,palette), 'palette-alpha':fixture(3,2,palette+chunk(b'tRNS',b'\0\xff')),
        'gray-low':fixture(0,1), 'rgb-trns':fixture(extra=chunk(b'tRNS',bytes(6))),
        'gray-trns':fixture(0,extra=chunk(b'tRNS',bytes(2))), 'adam7':fixture(interlace=1),
        'adam7-large':fixture(interlace=1,width=17,height=11),
        'sixteen':fixture(bits=16), 'icc':fixture(extra=chunk(b'iCCP',b'Test\0\0'+zlib.compress(profile, level=0))),
        'exif':fixture(extra=chunk(b'eXIf',b'II*\0\x08\0\0\0\0\0\0\0\0\0')),
        'bad-icc':fixture(extra=chunk(b'iCCP',b'Test\0\0'+zlib.compress(b'invalid'))),
        'broken':rgb[:20], 'bad-crc':rgb[:29]+bytes(4)+rgb[33:],
        'bad-palette':fixture(3,2), 'zero':fixture(width=0),
        'header-only':rgb[:41], # Signature + IHDR + first IDAT length/type, no payload.
    }
    for name,color,bits,transparent,interlaced in [
            ('gray-two',0,2,False,0),('gray-four',0,4,False,0),
            ('gray-low-trns',0,1,True,0),('palette-adam7',3,2,True,1),
            ('rgba-adam7',6,8,True,1)]:
        extra = palette+chunk(b'tRNS',b'\0\xff') if color == 3 else chunk(b'tRNS',bytes(2)) if color == 0 and transparent else b''
        fixtures[name] = fixture(color,bits,extra,interlaced)
        (directory/(name+'.raw')).write_bytes(expected_pixels(color,bits,transparent))
    fixtures['bad-idat'] = rgb[:41]+bytes([rgb[41]^255])+rgb[42:]
    fixtures['no-iend'] = rgb[:-12]
    fixtures['late-exif'] = rgb[:-12]+chunk(b'eXIf',b'II*\0\x08\0\0\0\0\0\0\0\0\0')+rgb[-12:]
    fixtures['apng'] = fixture(extra=chunk(b'acTL',struct.pack('>II',1,0)))
    fixtures['late-apng'] = rgb[:-12]+chunk(b'fcTL',bytes(26))+rgb[-12:]
    fixtures['cicp'] = fixture(extra=chunk(b'cICP',bytes([1,13,0,1])))
    fixtures['gamma'] = fixture(extra=chunk(b'gAMA',struct.pack('>I',45455)))
    for intent in range(4):
        name = 'srgb' if intent == 0 else f'srgb-{intent}'
        fixtures[name] = fixture(extra=chunk(b'sRGB',bytes([intent])))
        (directory/(name+'.raw')).write_bytes(expected_pixels())
    for name,color,bits,alpha in [('srgb-gray',0,8,False),('srgb-gray-alpha',4,8,True),
                                 ('srgb-rgba',6,8,True),('srgb-palette',3,2,True)]:
        extra = chunk(b'sRGB',bytes([2])) + (palette+chunk(b'tRNS',b'\0\xff') if color == 3 else b'')
        fixtures[name] = fixture(color,bits,extra)
        (directory/(name+'.raw')).write_bytes(expected_pixels(color,bits,alpha))
    fixtures['srgb-gamma'] = fixture(extra=chunk(b'gAMA',struct.pack('>I',100000))+chunk(b'sRGB',b'\0'))
    (directory/'srgb-gamma.raw').write_bytes(expected_pixels())
    gray_profile = bytearray(profile); gray_profile[16:20] = b'GRAY'
    fixtures['gray-icc'] = fixture(0,extra=chunk(b'iCCP',b'Test\0\0'+zlib.compress(gray_profile,level=0)))
    fixtures['gray-alpha-icc'] = fixture(4,extra=chunk(b'iCCP',b'Test\0\0'+zlib.compress(gray_profile,level=0)))
    fixtures['unknown'] = fixture(extra=chunk(b'vpAg',b'ignored'))
    fixtures['critical'] = fixture(extra=chunk(b'VpAg',b'unknown critical'))
    for name,color,bits,alpha in [('rgb',2,8,False),('gray',0,8,False),('rgba',6,8,True),
        ('gray-alpha',4,8,True),('palette',3,2,False),('palette-alpha',3,2,True),
        ('gray-low',0,1,False),('rgb-trns',2,8,True),('gray-trns',0,8,True),('adam7',2,8,False),
        ('icc',2,8,False),('gray-icc',0,8,False),('gray-alpha-icc',4,8,True),('unknown',2,8,False)]:
        (directory/(name+'.raw')).write_bytes(expected_pixels(color,bits,alpha))
    (directory/'adam7-large.raw').write_bytes(expected_pixels(width=17,height=11))
    for name, data in fixtures.items():
        (directory/(name+'.png')).write_bytes(data)
    subprocess.run([str(executable.resolve()), str(directory.resolve())], check=True)


if __name__ == '__main__':
    main()
