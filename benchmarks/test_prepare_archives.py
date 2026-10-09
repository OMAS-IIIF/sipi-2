#!/usr/bin/env python3
"""Offline preparation regressions; pass the compiled helper as the sole argument.

Covers odd edges, exact RGB8 preservation, full uint16 range, real transparency
rejection and refusal to overwrite. Does not require the private archive corpus.
"""
from array import array
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from PIL import Image
from prepare_archives import verify, file_sha

HELPER = Path(sys.argv.pop(1)).resolve()


class PreparationTests(unittest.TestCase):
    def test_rgb_odd_edges_and_overwrite(self):
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            size = (513,259)
            pixels = bytes((x*17+y*11+c*41)%256 for y in range(size[1]) for x in range(size[0]) for c in range(3))
            Image.frombytes('RGB',size,pixels).save(root/'source.tif')
            subprocess.run([HELPER,root/'source.tif',root/'out'],check=True,capture_output=True)
            result = verify(root/'out')
            with Image.open(root/'out/reference.tif') as reference:
                self.assertEqual(reference.tobytes(),pixels)
            self.assertEqual(result['levels'],[[513,259],[257,130],[129,65]])
            before = file_sha(root/'out/image.jp2')
            repeated = subprocess.run([HELPER,root/'source.tif',root/'out'],capture_output=True)
            self.assertNotEqual(repeated.returncode,0)
            self.assertEqual(file_sha(root/'out/image.jp2'),before)

    def test_gray_uint16_full_range(self):
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            values = [0,257,32768,65535]
            data = array('H',(values[x%4] for x in range(513*259)))
            # I;16 is explicitly little endian; retain portability to big-endian hosts.
            if sys.byteorder != 'little':
                data.byteswap()
            Image.frombytes('I;16',(513,259),data.tobytes()).save(root/'source.tif')
            subprocess.run([HELPER,root/'source.tif',root/'out'],check=True,capture_output=True)
            verify(root/'out')
            with Image.open(root/'out/reference.tif') as reference:
                self.assertEqual(list(reference.tobytes()[:4]),[0,1,128,255])

    def test_transparency_is_not_silently_discarded(self):
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            Image.new('RGBA',(513,259),(20,30,40,128)).save(root/'source.tif')
            original = file_sha(root/'source.tif')
            result = subprocess.run([HELPER,root/'source.tif',root/'out'],capture_output=True,text=True)
            self.assertNotEqual(result.returncode,0)
            self.assertIn('Nonopaque 8-bit alpha',result.stderr)
            self.assertFalse((root/'out/image.jp2').exists())
            self.assertEqual(file_sha(root/'source.tif'),original)


if __name__ == '__main__':
    unittest.main()
