"""Regression checks for result matching and paired timing aggregation."""
import copy
import unittest
from compare_resize import compare


class ComparisonTests(unittest.TestCase):
    def runs(self):
        case = {'kind': 'synthetic', 'case': 'sample', 'channels': 3,
                'fingerprint': '123', 'output_width': 2, 'output_height': 2,
                'source_width': 4, 'source_height': 4}
        runs = []
        for variant, time in [('before', 4), ('after', 2), ('after', 4), ('before', 8)]:
            c = dict(case, resize={'median_ms': time})
            runs.append({'variant': variant, 'report': {'cases': [c]}})
        return runs

    def test_paired_mean_of_medians(self):
        result = compare(self.runs())[0]['resize']
        self.assertEqual(result, {'before_ms': 6, 'after_ms': 3, 'speedup': 2})

    def test_reject_pixel_or_jpeg_mismatch(self):
        for field in ('fingerprint', 'jpeg_fingerprint', 'output_width'):
            runs = self.runs()
            runs[2]['report']['cases'][0][field] = 'changed'
            with self.assertRaisesRegex(ValueError, 'Output mismatch'):
                compare(runs)

    def test_reject_missing_or_duplicate_case(self):
        for duplicate in (False, True):
            runs = self.runs()
            cases = runs[1]['report']['cases']
            if duplicate:
                cases.append(copy.deepcopy(cases[0]))
            else:
                cases.clear()
            with self.assertRaises(ValueError):
                compare(runs)


if __name__ == '__main__':
    unittest.main()
