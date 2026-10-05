"""Independent RGB/RGBA8 PNG test decoder using only struct/zlib, without color conversion."""
import struct
import zlib


def decode_png(data):
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    chunks, compressed, profile = [], bytearray(), None
    offset = 8
    while offset < len(data):
        size, kind = struct.unpack_from('>I4s', data, offset)
        payload = data[offset+8:offset+8+size]
        crc, = struct.unpack_from('>I', data, offset+8+size)
        assert crc == zlib.crc32(kind + payload)
        chunks.append(kind)
        if kind == b'IHDR':
            width, height, depth, color, compression, filtering, interlace = struct.unpack('>IIBBBBB', payload)
            assert (depth, compression, filtering, interlace) == (8, 0, 0, 0) and color in (0,2,4,6)
            channels = {0:1,2:3,4:2,6:4}[color]
        elif kind == b'IDAT':
            compressed.extend(payload)
        elif kind == b'iCCP':
            _, contents = payload.split(b'\0', 1)
            assert contents[0] == 0
            profile = zlib.decompress(contents[1:])
        offset += size + 12
    assert offset == len(data) and chunks[0] == b'IHDR' and chunks[-1] == b'IEND'
    raw = zlib.decompress(compressed)
    stride = width * channels
    assert len(raw) == height*(stride+1)
    pixels, previous = bytearray(), bytearray(stride)
    for y in range(height):
        method = raw[y*(stride+1)]
        row = bytearray(raw[y*(stride+1)+1:(y+1)*(stride+1)])
        assert method in range(5)
        for x in range(stride):
            a, b, c = (row[x-channels] if x >= channels else 0), previous[x], (previous[x-channels] if x >= channels else 0)
            p = a+b-c
            distances = (abs(p-a), abs(p-b), abs(p-c))
            paeth = (a, b, c)[distances.index(min(distances))]
            predictor = (0, a, b, (a+b)//2, paeth)[method]
            row[x] = (row[x]+predictor) & 255
        pixels.extend(row)
        previous = row
    return width, height, bytes(pixels), profile, chunks


if __name__ == '__main__':
    import pathlib
    import subprocess
    import sys
    import tempfile
    with tempfile.TemporaryDirectory() as directory:
        subprocess.run([sys.argv[1], directory], check=True)
        root = pathlib.Path(directory)
        expected = bytearray()
        random = 17
        for _ in range(257*129*3):
            random = (random*1664525 + 1013904223) & 0xffffffff
            expected.append(random >> 24)
        for name in ('untagged', 'profile'):
            w, h, pixels, profile, chunks = decode_png((root/(name+'.png')).read_bytes())
            assert (w, h, pixels) == (257, 129, expected)
            assert b'sRGB' not in chunks and b'gAMA' not in chunks and b'cHRM' not in chunks
            if name == 'untagged':
                assert profile is None
            else:
                header = bytearray(132)
                for offset, value in {0:132, 8:0x04000000, 12:0x6d6e7472, 16:0x52474220,
                                      20:0x58595a20, 36:0x61637370, 68:0xf6d6, 72:0x10000, 76:0xd32d}.items():
                    struct.pack_into('>I', header, offset, value)
                assert profile == header
        assert decode_png((root/'rgba.png').read_bytes())[:3] == (2,1,bytes([255,0,0,128,0,0,255,0]))
        assert decode_png((root/'gray.png').read_bytes())[:3] == (3,1,bytes([0,127,255]))
        assert decode_png((root/'gray.png').read_bytes())[3][16:20] == b'GRAY'
        assert decode_png((root/'one.png').read_bytes())[:3] == (1,1,bytes([0,127,255]))
        assert decode_png((root/'column.png').read_bytes())[:3] == (1,3,bytes(range(1,10)))
        assert decode_png((root/'row.png').read_bytes())[:3] == (3,1,bytes(range(1,10)))
    print('Independent PNG pixel, chunk CRC, ICC and untagged round trips passed')
