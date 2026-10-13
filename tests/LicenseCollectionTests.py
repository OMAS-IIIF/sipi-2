"""License packaging regression tests using isolated package/source fixtures."""
from pathlib import Path
import hashlib
import json
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'scripts'))
from collect_licenses import collect


class LicenseCollectionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.package = self.root/'package'
        (self.package/'licenses').mkdir(parents=True)
        (self.package/'licenses/LICENSE').write_bytes(b'Original license\n')
        for name in ('LICENSE', 'LICENSING.md', 'THIRD_PARTY_NOTICES.md'):
            (self.root/name).write_text(name)
        for directory in ('LICENSES', 'third_party/compact-icc'):
            (self.root/directory).mkdir(parents=True)
            (self.root/directory/'LICENSE').write_text('terms')
        self.extra = self.root/'third_party/notices'
        self.extra.mkdir(parents=True)
        (self.extra/'NOTICE').write_bytes(b'Original notice\n')
        self.manifest = {'files': [{'reference': 'fixture/1.0#abc', 'file': 'NOTICE',
                          'sha256': hashlib.sha256(b'Original notice\n').hexdigest()}]}
        self.node = {'name': 'fixture', 'version': '1.0', 'context': 'host',
                     'ref': 'fixture/1.0#abc', 'license': 'MIT',
                     'package_folder': str(self.package), 'package_type': 'header-library'}

    def run_collection(self):
        graph = self.root/'graph.json'
        graph.write_text(json.dumps({'graph': {'nodes': {
            '1': self.node, '2': {'name': 'build-only', 'context': 'build'}}}}))
        (self.extra/'manifest.json').write_text(json.dumps(self.manifest))
        return collect(graph, self.root/'output', self.root)

    def test_header_license_supplement_and_no_cache_path_leak(self):
        inventory = self.run_collection()
        self.assertEqual(list(inventory['components']), ['fixture'])
        self.assertNotIn(str(self.root), json.dumps(inventory))
        self.assertEqual((self.root/'output/dependencies/fixture/LICENSE').read_bytes(),
                         b'Original license\n')
        for path, digest in inventory['files'].items():
            self.assertEqual(hashlib.sha256((self.root/'output'/path).read_bytes()).hexdigest(), digest)

    def test_missing_package_license_fails(self):
        (self.package/'licenses/LICENSE').unlink()
        with self.assertRaisesRegex(ValueError, 'Missing license'):
            self.run_collection()

    def test_changed_recipe_requires_notice_review(self):
        self.node['ref'] = 'fixture/1.0#changed'
        with self.assertRaisesRegex(ValueError, 'changed dependency'):
            self.run_collection()

    def test_modified_supplement_fails(self):
        (self.extra/'NOTICE').write_text('changed')
        with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
            self.run_collection()

    def test_platform_specific_supplement_is_omitted_when_dependency_absent(self):
        self.manifest['files'].append({'reference': 'linux-only/1.0#abc',
                                       'file': 'unavailable', 'sha256': 'unused'})
        inventory = self.run_collection()
        self.assertEqual(list(inventory['components']), ['fixture'])
        self.assertNotIn('supplemental/unavailable', inventory['files'])

    def test_existing_evidence_is_not_overwritten(self):
        self.run_collection()
        with self.assertRaises(FileExistsError):
            self.run_collection()


if __name__ == '__main__':
    unittest.main()
