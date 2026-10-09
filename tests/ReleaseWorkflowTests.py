"""Release orchestration checks; temporary versions and mocked Docker, never a push."""
from pathlib import Path
import contextlib
import io
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


class DockerWorkflowTests(unittest.TestCase):
    def execute(self, action, failure=None):
        """Record argv sequencing, simulating one explicit command failure if requested."""
        calls=[]
        def command(args, **kwargs):
            calls.append((args,kwargs))
            if failure and failure(args):
                raise subprocess.CalledProcessError(1,args)
        with patch.object(sys,'argv',['docker_image.py',action,'--image','lrosenth/sipi-2']), \
             patch.object(docker_image,'read_version',return_value='2.3.4'), \
             patch.object(docker_image.subprocess,'run',side_effect=command), \
             patch.object(docker_image.subprocess,'check_output',return_value='aarch64\n'), \
             contextlib.redirect_stdout(io.StringIO()):
            if failure:
                with self.assertRaises(subprocess.CalledProcessError): docker_image.main()
            else:
                docker_image.main()
        return calls

    def test_publish_follows_both_platform_smokes(self):
        calls=self.execute('publish')
        self.assertEqual(len(calls),5)
        for index,platform in [(0,'linux/amd64'),(2,'linux/arm64')]:
            build=calls[index][0]
            self.assertEqual(build[build.index('--platform')+1],platform)
            self.assertIn('--load',build)
            self.assertEqual(calls[index+1][1]['env']['DOCKER_DEFAULT_PLATFORM'],platform)
        publish=calls[-1][0]
        self.assertIn('--push',publish)
        self.assertIn('lrosenth/sipi-2:2.3.4',publish)
        self.assertIn('lrosenth/sipi-2:latest',publish)
        self.assertIn('linux/amd64,linux/arm64',publish)

    def test_failed_smoke_prevents_publication(self):
        calls=self.execute('publish',lambda argv: any('docker_smoke.py' in a for a in argv))
        self.assertEqual(len(calls),2)
        self.assertFalse(any('--push' in args for args,_ in calls))

    def test_local_and_test_never_publish(self):
        for action, count in [('local',2),('test',4)]:
            calls=self.execute(action)
            self.assertEqual(len(calls),count)
            self.assertFalse(any('--push' in args for args,_ in calls))


if __name__=='__main__':
    unittest.main()
