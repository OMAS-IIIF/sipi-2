"""Release orchestration checks; temporary versions and mocked Docker, never a push."""
from pathlib import Path
import contextlib
import io
import json
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'scripts'))
import version
import docker_image


class VersionTests(unittest.TestCase):
    def test_bumps_preserve_other_content(self):
        for part, expected in [('patch','2.9.10'),('minor','2.10.0'),('major','3.0.0')]:
            with self.subTest(part=part), tempfile.TemporaryDirectory() as temporary:
                path=Path(temporary)/'CMakeLists.txt'
                text='# Keep me\nproject(sipi-2 VERSION 2.9.9 LANGUAGES CXX)\n# Also keep me\n'
                path.write_text(text); path.chmod(0o640)
                self.assertEqual(version.bump(path,part),expected)
                self.assertEqual(path.read_text(),text.replace('2.9.9',expected))
                self.assertEqual(version.read_version(path),expected)
                self.assertEqual(path.stat().st_mode & 0o777,0o640)
                self.assertEqual(len(list(path.parent.iterdir())),1)

    def test_invalid_source_is_unchanged(self):
        declaration='project(sipi-2 VERSION 0.1.0 LANGUAGES CXX)\n'
        for text in ['',declaration*2,declaration.replace('0.1.0','0.01.0')]:
            with self.subTest(text=text), tempfile.TemporaryDirectory() as temporary:
                path=Path(temporary)/'CMakeLists.txt'; path.write_text(text)
                with self.assertRaises(ValueError): version.bump(path,'patch')
                self.assertEqual(path.read_text(),text)


class SourceIdentityTests(unittest.TestCase):
    def test_real_git_state_includes_all_pending_sources(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            def git(*args):
                return subprocess.check_output(['git', *args], cwd=root, stderr=subprocess.DEVNULL)
            git('init'); (root/'tracked').write_text('original')
            (root/'.gitignore').write_text('build/\n')
            git('add', '.'); git('-c','user.name=Test','-c','user.email=test@example.invalid','commit','-m','fixture')
            with patch.object(docker_image, 'ROOT', root):
                identity = docker_image.source_identity()
                self.assertFalse(identity['dirty'])
                (root/'build').mkdir(); (root/'build/log').write_text('ignored')
                self.assertEqual(docker_image.source_identity(), identity)
                (root/'untracked').write_text('pending')
                self.assertTrue(docker_image.source_identity()['dirty'])
                (root/'untracked').unlink(); (root/'tracked').write_text('modified')
                self.assertTrue(docker_image.source_identity()['dirty'])
                git('add','tracked')
                self.assertTrue(docker_image.source_identity()['dirty'])


class DockerWorkflowTests(unittest.TestCase):
    def execute(self, action, failure=None, dirty=False, changed=False, bad_labels=False):
        """Exercise orchestration and reports with fake Docker; never publish anything."""
        calls = []; identity = {'revision': 'a'*40, 'dirty': dirty}; checks = 0
        def source():
            nonlocal checks
            checks += 1
            return {**identity, 'revision': 'b'*40} if changed and checks >= 5 else identity
        def command(args, **kwargs):
            calls.append((args, kwargs))
            if failure and failure(args):
                raise subprocess.CalledProcessError(1, args)
            if args[:2] == ['git', 'archive']:
                Path(next(a.split('=',1)[1] for a in args if a.startswith('--output='))).write_bytes(b'fixed archive')
            if '--metadata-file' in args:
                Path(args[args.index('--metadata-file')+1]).write_text(json.dumps({'containerimage.digest': 'sha256:'+'d'*64}))
            if args[:3] == ['docker','buildx','build'] and action == 'publish':
                self.assertEqual(kwargs['stdin'].read(), b'fixed archive')
        def output(args, **kwargs):
            if args[:2] == ['docker', 'info']: return 'aarch64\n'
            if args[:3] == ['docker','image','inspect']:
                architecture = args[-1].rsplit('-',1)[1]
                labels = docker_image.image_labels('2.3.4', identity)
                if bad_labels: labels['org.opencontainers.image.revision'] = 'wrong'
                return json.dumps([{'Os':'linux','Architecture':architecture,'Id':'sha256:'+'c'*64,'Config':{'Labels':labels}}])
            raise AssertionError(args)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with patch.object(sys,'argv',['docker_image.py',action,'--image','lrosenth/sipi-2']), \
                 patch.object(docker_image,'ROOT',root), \
                 patch.object(docker_image,'source_identity',side_effect=source), \
                 patch.object(docker_image,'read_version',return_value='2.3.4'), \
                 patch.object(docker_image.subprocess,'run',side_effect=command), \
                 patch.object(docker_image.subprocess,'check_output',side_effect=output), \
                 contextlib.redirect_stdout(io.StringIO()):
                if failure:
                    with self.assertRaises(subprocess.CalledProcessError): docker_image.main()
                elif changed or bad_labels or (dirty and action == 'publish'):
                    with self.assertRaises(ValueError): docker_image.main()
                else:
                    docker_image.main()
            reports = list(root.glob('build/releases/*/report.json'))
            report = json.loads(reports[0].read_text()) if reports else None
        return calls, report

    def test_publish_follows_both_platform_smokes_with_fixed_archive(self):
        calls, report = self.execute('publish')
        self.assertEqual(calls[0][0][:2], ['git','archive'])
        self.assertEqual(calls[0][0][-1], 'a'*40)
        for index, platform in [(1,'linux/amd64'),(3,'linux/arm64')]:
            build = calls[index][0]
            self.assertEqual(build[build.index('--platform')+1], platform)
            self.assertEqual(build[-1], '-')
            self.assertIn('--load', build)
            self.assertIn('org.opencontainers.image.version=2.3.4', build)
            self.assertEqual(calls[index+1][1]['env']['DOCKER_DEFAULT_PLATFORM'], platform)
        publish = calls[-1][0]
        self.assertIn('--push', publish)
        self.assertIn('lrosenth/sipi-2:2.3.4', publish)
        self.assertIn('lrosenth/sipi-2:latest', publish)
        self.assertEqual(report['published_digest'], 'sha256:'+'d'*64)
        self.assertEqual(report['publication'], 'pushed')
        self.assertEqual([p['smoke'] for p in report['platforms']], ['passed','passed'])

    def test_failed_smoke_prevents_publication_and_records_failure(self):
        calls, report = self.execute('publish', lambda a: any('docker_smoke.py' in x for x in a))
        self.assertFalse(any('--push' in a for a,_ in calls))
        self.assertEqual(report['publication'], 'not attempted')
        self.assertIn('failed', report['status'])
        self.assertEqual(report['platforms'][0]['smoke'], 'pending')

    def test_failed_push_retains_uncertainty_in_report(self):
        calls, report = self.execute('publish', lambda a: '--push' in a)
        self.assertTrue(any('--push' in a for a,_ in calls))
        self.assertIn('failed', report['status'])
        self.assertIn('attempting', report['publication'])
        self.assertNotIn('published_digest', report)

    def test_dirty_publish_rejected_before_any_build(self):
        calls, report = self.execute('publish', dirty=True)
        self.assertEqual(calls, [])
        self.assertIsNone(report)

    def test_source_change_or_bad_labels_prevent_push(self):
        for options in ({'changed':True}, {'bad_labels':True}):
            with self.subTest(options=options):
                calls, report = self.execute('publish', **options)
                self.assertFalse(any('--push' in a for a,_ in calls))
                self.assertIn('failed', report['status'])

    def test_local_and_test_never_publish_and_mark_dirty_sources(self):
        for action, count in [('local',2),('test',4)]:
            calls, report = self.execute(action, dirty=True)
            self.assertEqual(len(calls), count)
            self.assertFalse(any('--push' in a for a,_ in calls))
            self.assertIn('org.opencontainers.image.revision='+'a'*40+'-dirty', calls[0][0])
            self.assertTrue(report['dirty'])
            self.assertEqual(report['status'], 'passed')

    def test_invalid_build_metadata_fails_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)/'metadata.json'
            for value in ({}, {'containerimage.digest':'not-a-digest'}):
                path.write_text(json.dumps(value))
                with self.assertRaises(ValueError): docker_image.build_metadata(path)


if __name__=='__main__':
    unittest.main()
