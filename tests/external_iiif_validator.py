"""Run the unmodified IIIF consortium validator against a disposable local server.

Arguments: sipi executable, validator checkout, validator venv Python, report path.
The checkout's official PNG reference image is copied, never altered. Reports
retain upstream output, commit, dependency versions and separate level2/optional
results. This explicit integration check is not part of offline CTest runs.
"""
from pathlib import Path
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from cache_http import server


def main():
    executable, checkout, python, report = map(lambda p: Path(p).absolute(), sys.argv[1:])
    commit = subprocess.check_output(['git', '-C', str(checkout), 'rev-parse', 'HEAD'], text=True).strip()
    if subprocess.check_output(['git', '-C', str(checkout), 'status', '--porcelain'], text=True).strip():
        raise RuntimeError('Use an unmodified validator checkout')
    identifier = '67352ccc-d1b0-11e1-89ae-279075081939.png'
    fixture = checkout/'html'/identifier
    evidence = {'validator': 'https://github.com/IIIF/image-validator', 'commit': commit,
                'image_sha256': hashlib.sha256(fixture.read_bytes()).hexdigest(),
                'dependencies': subprocess.check_output([str(python), '-m', 'pip', 'freeze'], text=True).splitlines(),
                'api': '3.0', 'server_sha256': hashlib.sha256(executable.read_bytes()).hexdigest(), 'runs': []}
    env = {k: v for k, v in os.environ.items() if not k.startswith('SIPI_')}
    with tempfile.TemporaryDirectory(prefix='sipi-external-validator-') as temporary:
        root = Path(temporary)
        (root/'images').mkdir()
        shutil.copyfile(fixture, root/'images'/identifier)
        (root/'policy.lua').write_text("function preflight(r) return {decision='allow'} end\n")
        with server(executable, root) as (port, logs):
            common = [str(python), str(checkout/'iiif-validate.py'), '--version=3.0',
                      '--server', f'127.0.0.1:{port}', '--prefix', 'external',
                      '--identifier', identifier, '--verbose']
            optional = ['linkheader_canonical', 'linkheader_profile', 'rot_mirror',
                        'rot_mirror_180', 'rot_full_non90', 'rot_region_non90',
                        'quality_grey', 'quality_bitonal']
            for name, args in [('level2', ['--level=2']),
                               ('supported_optional', [a for test in optional for a in ('--test', test)])]:
                completed = subprocess.run(common + args, cwd=checkout, env=env,
                                           capture_output=True, text=True, timeout=180)
                summary = re.search(r'Done \((\d+) tests, (\d+) failures\)', completed.stderr)
                if not summary or int(summary[1]) == 0:
                    raise RuntimeError('Validator did not complete any tests: ' + completed.stderr)
                if name == 'supported_optional' and int(summary[1]) != len(optional):
                    raise RuntimeError('Validator did not run every selected optional test')
                evidence['runs'].append({'name': name, 'command': common + args,
                    'exit_code': completed.returncode, 'stdout': completed.stdout, 'stderr': completed.stderr})
                print(name, 'exit', completed.returncode, flush=True)
        evidence['server_log'] = logs[0]
    report.parent.mkdir(parents=True, exist_ok=True)
    report.write_text(json.dumps(evidence, indent=2) + '\n')
    return int(any(r['exit_code'] for r in evidence['runs']))


if __name__ == '__main__':
    sys.exit(main())
