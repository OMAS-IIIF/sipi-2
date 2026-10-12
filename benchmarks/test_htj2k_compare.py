"""Reject unequal coding settings and invalid codec comparison output."""
import copy
import unittest
from htj2k_compare import validate_coding, validate_runs


class ValidationTests(unittest.TestCase):
    def test_reject_non_ht_or_unequal_settings(self):
        common = dict(bits=[8]*3, signed=[False]*3, tile_size=[1024]*2,
                      progression=0, layers=1, mct=1, resolutions=6,
                      codeblock=[64]*2, reversible_5_3=True, colour_method=2, icc_sha256='same')
        ht = dict(common, brand='jph ', block_style=64, rsiz=0x4000, cap='000200000006')
        validate_coding(common, ht)
        for field, value in (('block_style',0), ('icc_sha256','different'), ('resolutions',5)):
            with self.subTest(field=field), self.assertRaises(ValueError):
                validate_coding(common, dict(ht, **{field:value}))

    def test_reject_missing_or_mismatched_output(self):
        cases = [dict(kind='synthetic', case=str(n), channels=3, fingerprint='s') for n in range(15)]
        for suffix in ('tif','jp2','jph'):
            for request in ('thumbnail','preview','tile'):
                cases.append(dict(kind='pipeline', file=f'/corpus/example/image.{suffix}',
                                  case=request, channels=3, fingerprint='p', jpeg_fingerprint='j',
                                  output_width=3, output_height=3, level_width=8, level_height=8,
                                  decoded_width=8, decoded_height=8))
        run={'cases':cases}
        validate_runs([run,copy.deepcopy(run)], ['example'])
        changed=copy.deepcopy(run)
        changed['cases'][-1]['jpeg_fingerprint']='changed'
        with self.assertRaisesRegex(ValueError,'JP2/JPH mismatch'):
            validate_runs([changed], ['example'])
        with self.assertRaisesRegex(ValueError,'Incomplete'):
            validate_runs([{'cases':cases[:-1]}], ['example'])


if __name__ == '__main__':
    unittest.main()
