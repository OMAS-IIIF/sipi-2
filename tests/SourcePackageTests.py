"""Source-release guards and graph/image consistency; no downloads or publication."""
from pathlib import Path
import json
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'scripts'))
import source_package


class SourcePackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.reference = 'library/1.0#abc'
        self.components = {'library': {'reference': self.reference, 'package_revision': 'original'}}
        self.identity = {'revision': 'a'*40, 'dirty': False}
        for arch in ('amd64', 'arm64'):
            folder = self.root/arch
            (folder/'licenses').mkdir(parents=True)
            (folder/'dependencies.json').write_text(json.dumps({'dependencies': [
                {'reference': self.reference, 'contexts': ['host'], 'sources_included': True}]}))
            (folder/'recipes.json').write_text(json.dumps({'Local Cache': {
                'library/1.0': {'revisions': {'abc': {}}}}}))
            (folder/'licenses/inventory.json').write_text(json.dumps({'components': self.components}))
            (folder/'conan-sources.tgz').write_bytes(b'fixture')
            (folder/'conan.lock').write_text('{}')
        self.report = {'status': 'passed', **self.identity, 'version': '1.0.0', 'platforms': [
            {'platform': 'linux/'+a, 'image_id': 'sha256:'+a, 'smoke': 'passed'}
            for a in ('amd64', 'arm64')]}

    def test_complete_export(self):
        self.assertEqual(len(source_package.validate_export(self.root/'arm64')['dependencies']), 1)

    def test_missing_library_source_rejected(self):
        p = self.root/'arm64/dependencies.json'
        data = json.loads(p.read_text())
        data['dependencies'][0]['sources_included'] = False
        p.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, 'all host'):
            source_package.validate_export(self.root/'arm64')

    def test_binary_recipe_selection_rejected(self):
        p = self.root/'arm64/recipes.json'
        data = json.loads(p.read_text())
        data['Local Cache']['library/1.0']['revisions']['abc']['packages'] = {'binary-id': {}}
        p.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, 'Binary packages'):
            source_package.validate_export(self.root/'arm64')

    def test_missing_runtime_dependency_rejected(self):
        (self.root/'arm64/licenses/inventory.json').write_text('{"components": {}}')
        with self.assertRaisesRegex(ValueError, 'graphs differ'):
            source_package.validate_export(self.root/'arm64')

    def verify_report(self, components):
        path = self.root/'report.json'
        path.write_text(json.dumps(self.report))
        with patch.object(source_package.docker_image, 'inspect_verified_image',
                          side_effect=lambda image_id, *_: image_id), \
             patch.object(source_package.subprocess, 'check_output',
                          return_value=json.dumps({'components': components})) as command:
            source_package.verify_image_report(path, self.identity, '1.0.0', self.root)
            self.assertEqual(command.call_count, 2)
            self.assertIn('sha256:arm64', command.call_args.args[0])

    def test_image_report_uses_immutable_ids(self):
        self.verify_report(self.components)

    def test_wrong_image_dependency_revision_rejected(self):
        with self.assertRaisesRegex(ValueError, 'differs from verified'):
            self.verify_report({'library': {'reference': self.reference, 'package_revision': 'other'}})

    def test_dirty_or_other_commit_report_rejected(self):
        for change in ({'dirty': True}, {'revision': 'b'*40}, {'version': '0.1.3'}):
            with self.subTest(change=change):
                previous = self.report.copy()
                self.report.update(change)
                with self.assertRaisesRegex(ValueError, 'clean version/commit'):
                    self.verify_report(self.components)
                self.report = previous

    def test_dirty_source_fails_before_docker_or_output(self):
        with patch.object(source_package.docker_image, 'source_identity',
                          return_value={'revision': 'a'*40, 'dirty': True}), \
             patch.object(source_package.subprocess, 'run') as command:
            with self.assertRaisesRegex(ValueError, 'clean commit'):
                source_package.package(self.root/'output')
            command.assert_not_called()
            self.assertFalse((self.root/'output').exists())


if __name__ == '__main__':
    unittest.main()
