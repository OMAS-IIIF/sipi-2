"""Independent PNG coverage fixtures; no image-library Python dependency."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib
from png_metadata import fixture, chunk, expected_pixels


def main():
    with tempfile.TemporaryDirectory(prefix='sipi-png-mask-') as temporary:
        root = Path(temporary)
        for bits in (1, 2, 4, 8):
            for interlace in (0, 1):
                name = f'gray-{bits}-{interlace}'
                (root / f'{name}.png').write_bytes(fixture(0, bits, interlace=interlace, width=17, height=11))
                (root / f'{name}.raw').write_bytes(expected_pixels(0, bits, width=17, height=11))
        profile = bytearray(132)
        for offset, value in {0:132, 8:0x04000000, 12:0x6d6e7472, 16:0x47524159,
                              20:0x58595a20, 36:0x61637370, 68:0xf6d6, 72:0x10000, 76:0xd32d}.items():
            struct.pack_into('>I', profile, offset, value)
        gray = fixture(0)
        cases = {
            'rgb': fixture(2), 'rgba': fixture(6), 'gray-alpha': fixture(4),
            'palette': fixture(3, 2, extra=chunk(b'PLTE', b'\0\0\0\xff\xff\xff')), 'sixteen': fixture(0, 16),
            'trns': fixture(0, extra=chunk(b'tRNS', bytes(2))),
            'icc': fixture(0, extra=chunk(b'iCCP', b'Mask\0\0' + zlib.compress(profile, level=0))),
            'chrm': fixture(0, extra=chunk(b'cHRM', struct.pack('>8I', 31270,32900,64000,33000,30000,60000,15000,6000))),
            'cicp': fixture(0, extra=chunk(b'cICP', bytes([1,13,0,1]))),
            'bad-idat': gray[:41] + bytes([gray[41] ^ 255]) + gray[42:],
            'gamma': fixture(0, extra=chunk(b'gAMA', struct.pack('>I', 45455))),
            'srgb': fixture(0, extra=chunk(b'sRGB', b'\0')),
            'exif': fixture(0, extra=chunk(b'eXIf', b'II*\x00\x08\x00\x00\x00\x00\x00')),
            'apng': fixture(0, extra=chunk(b'acTL', struct.pack('>II', 1, 0))),
            'late-apng': gray[:-12] + chunk(b'fcTL', bytes(26)) + gray[-12:],
            'late-gamma': gray[:-12] + chunk(b'gAMA', struct.pack('>I', 45455)) + gray[-12:],
            'truncated': gray[:-15], 'missing-iend': gray[:-12],
            'bad-crc': gray[:-1] + bytes([gray[-1] ^ 1]),
        }
        for name, data in cases.items():
            (root / f'{name}.png').write_bytes(data)
        subprocess.run([str(Path(sys.argv[1]).resolve()), str(root)], check=True)


if __name__ == '__main__':
    main()
