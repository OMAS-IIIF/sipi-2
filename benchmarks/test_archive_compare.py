#!/usr/bin/env python3
"""Workload/accounting regressions; no Docker or private image corpus required."""
from collections import Counter
import hashlib
from unittest.mock import patch
import unittest

from archive_compare import requests_for, jpeg_variants, check_sampling
from server_compare import batch


class ArchiveComparisonTests(unittest.TestCase):
    def test_jpeg_variants_keep_baseline_compatible(self):
        variants = jpeg_variants('old-image', 'new-image')
        self.assertIsNone(variants['ijg-444']['jpeg_options'])
        self.assertEqual(variants['ijg-444']['image'], 'old-image')
        self.assertEqual(variants['turbo-444']['image'], variants['turbo-420']['image'])
        self.assertEqual(variants['turbo-444']['jpeg_options'], (90, '4:4:4'))
        self.assertEqual(variants['turbo-420']['jpeg_options'], (90, '4:2:0'))

    def test_sampling_is_checked_from_components(self):
        color = {'output_format': 'jpg', 'decoded_mode': 'RGB', 'path': '/color',
                 'sampling': [(1, 2, 2, 0), (2, 1, 1, 1), (3, 1, 1, 1)]}
        gray = {'output_format': 'jpg', 'decoded_mode': 'L', 'path': '/gray',
                'sampling': [(1, 1, 1, 0)]}
        check_sampling([color, gray], '4:2:0')
        check_sampling([gray, {'output_format': 'png'}], '4:4:4')
        with self.assertRaisesRegex(ValueError, 'Unexpected JPEG sampling'):
            check_sampling([color], '4:4:4')

    def test_regions_and_dimensions(self):
        """Exercise small odd, landscape and very large source geometry."""
        for width, height in ((1671, 2212), (3684, 2444), (18080, 12640)):
            requests = requests_for('image', width, height, 'jp2', 'png')
            self.assertEqual(len({r['path'] for r in requests}), 5)
            for request in requests:
                x, y, w, h = request['region']
                self.assertGreaterEqual(min(x, y), 0)
                self.assertLessEqual(x + w, width)
                self.assertLessEqual(y + h, height)
                self.assertLessEqual(max(request['size']), 2048)
                self.assertTrue(request['path'].startswith('/image.jp2/'))
                self.assertTrue(request['path'].endswith('/default.png'))
                if request['native']:
                    self.assertEqual((w, h), request['size'])
            edge = next(r for r in requests if r['category'] == 'native-edge')
            x, y, w, h = edge['region']
            self.assertEqual((x + w, y + h), (width, height))

    def test_balanced_rounds_and_legacy_accounting(self):
        """Each balanced client visits every URL; legacy striding visits once."""
        paths = ['/slow-image', '/fast-image', '/map']
        body = b'checked response'
        expected = {path: ('image/png', hashlib.sha256(body).hexdigest()) for path in paths}
        with patch('server_compare.http.client.HTTPConnection'), \
                patch('server_compare.fetch', return_value=('image/png', body)):
            result = batch(0, paths, 2, expected, balanced=True, record_paths=True)
            self.assertEqual(result['requests'], 6)
            self.assertEqual(Counter(r[2] for r in result['records_ms_bytes']),
                             Counter({path: 2 for path in paths}))
            self.assertEqual(result['bytes'], 6 * len(body))
            legacy = batch(0, paths, 2, expected)
            self.assertEqual(legacy['requests'], 3)
            self.assertTrue(all(len(r) == 2 for r in legacy['records_ms_bytes']))

    def test_changed_response_fails_measurement(self):
        """Never turn a successful HTTP response with wrong pixels into a rate."""
        expected = {'/image': ('image/png', hashlib.sha256(b'original').hexdigest())}
        with patch('server_compare.http.client.HTTPConnection'), \
                patch('server_compare.fetch', return_value=('image/png', b'changed')):
            with self.assertRaisesRegex(RuntimeError, 'Response changed'):
                batch(0, ['/image'], 1, expected, balanced=True)


if __name__ == '__main__':
    unittest.main()
