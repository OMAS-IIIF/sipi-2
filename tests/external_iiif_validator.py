"""Run the unmodified IIIF consortium validator against a disposable local server.

Arguments: sipi executable, validator checkout, validator venv Python, report path.
The official reference stays unchanged. A second scenario uses a nearest-neighbor
2x source and a reduced public service whose pixels must equal that reference.
Reports retain provenance and separate level2/optional results for both services. This explicit integration check is not part of offline CTest runs.
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
from cache_http import server, get


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
    for scenario in ('original', 'restricted_public'):
        with tempfile.TemporaryDirectory(prefix='sipi-external-validator-') as temporary:
            root = Path(temporary)
            (root/'images').mkdir()
            source = root/'images'/identifier
            if scenario == 'original':
                shutil.copyfile(fixture, source)
                policy = "function preflight(r) return {decision='allow'} end\n"
                prefix = 'external'
            else:
                # Upstream assertions require its 1000x1000 pattern. Duplicate
                # each pixel 2x2 so real full-image area reduction restores it.
                subprocess.run([str(python), '-c',
                    "from PIL import Image; import sys; "
                    "im=Image.open(sys.argv[1]); "
                    "im.resize((2000,2000),Image.Resampling.NEAREST).save(sys.argv[2])",
                    str(fixture), str(source)], check=True)
                policy = """function preflight(r)
                  if r.prefix[1]~='public' then return {decision='deny'} end
                  return {decision='allow',resolution={max_width=1000,max_height=1000}}
                end"""
                prefix = 'public'
            (root/'policy.lua').write_text(policy)
            context = {'scenario': scenario, 'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
                       'source_dimensions': [1000,1000] if scenario=='original' else [2000,2000],
                       'public_dimensions': [1000,1000], 'policy': policy}
            with server(executable, root) as (port, logs):
                status, _, body = get(port, f'/{prefix}/{identifier}/info.json')
                info = json.loads(body)
                if status != 200 or (info['width'],info['height']) != (1000,1000):
                    raise RuntimeError('Reference service dimensions do not match validator contract')
                context['info'] = info
                if scenario == 'restricted_public':
                    if get(port, f'/{identifier}/full/max/0/default.png')[0] != 403:
                        raise RuntimeError('Original alias must remain protected')
                    status, _, pixels = get(port, f'/{prefix}/{identifier}/full/max/0/default.png')
                    if status != 200:
                        raise RuntimeError('Public raster could not be generated')
                    rendered = root/'public.png'
                    rendered.write_bytes(pixels)
                    subprocess.run([str(python), '-c',
                        "from PIL import Image; import sys; "
                        "a=Image.open(sys.argv[1]).convert('RGBA'); b=Image.open(sys.argv[2]).convert('RGBA'); "
                        "assert a.size==b.size and a.tobytes()==b.tobytes(), 'Public pixels differ from reference'",
                        str(fixture), str(rendered)], check=True)
                    context['public_pixels_equal_reference'] = True
                # The pinned upstream WebP test compares bytes to str under Python
                # 3 and cannot pass. Keep upstream untouched; independently verify
                # its intended format contract and actual decodability here.
                status, headers, webp = get(port, f'/{prefix}/{identifier}/full/max/0/default.webp')
                if status != 200 or headers.get('content-type','').split(';')[0] != 'image/webp':
                    raise RuntimeError('WebP response contract failed')
                if webp[:4] != b'RIFF' or webp[8:12] != b'WEBP':
                    raise RuntimeError('Invalid WebP signature')
                webp_file = root/'public.webp'
                webp_file.write_bytes(webp)
                subprocess.run([str(python), '-c',
                    "from PIL import Image; import sys; im=Image.open(sys.argv[1]); "
                    "im.load(); assert im.format=='WEBP' and im.size==(1000,1000)",
                    str(webp_file)], check=True)
                context['independent_webp_check'] = {'passed': True,
                    'checks': ['HTTP 200', 'image/webp', 'RIFF/WEBP signature', 'Pillow full decode', '1000x1000'],
                    'upstream_exclusion': 'Pinned format_webp compares img[8:12] bytes to str WEBP under Python 3'}
                common = [str(python), str(checkout/'iiif-validate.py'), '--version=3.0',
                          '--server', f'127.0.0.1:{port}', '--prefix', prefix,
                          '--identifier', identifier, '--verbose']
                optional = ['linkheader_canonical', 'linkheader_profile', 'rot_mirror',
                            'rot_mirror_180', 'rot_full_non90', 'rot_region_non90',
                            'quality_grey', 'quality_bitonal']
                for name, args in [('level2', ['--level=2']),
                                   ('supported_optional', [a for test in optional for a in ('--test', test)])]:
                    completed = subprocess.run(common + args, cwd=checkout, env=env,
                                               capture_output=True, text=True, timeout=180)
                    summary = re.search(r'Done \((\d+) tests, (\d+) failures\)', completed.stderr)
                    expected = 31 if name == 'level2' else len(optional)
                    if not summary or int(summary[1]) != expected:
                        raise RuntimeError('Validator did not complete every selected test: ' + completed.stderr)
                    evidence['runs'].append({'name': name, **context, 'command': common + args,
                        'tests': int(summary[1]), 'failures': int(summary[2]),
                        'exit_code': completed.returncode, 'stdout': completed.stdout, 'stderr': completed.stderr})
                    print(scenario, name, 'tests', summary[1], 'failures', summary[2],
                          'exit', completed.returncode, flush=True)
            evidence.setdefault('server_logs', {})[scenario] = logs[0]
    report.parent.mkdir(parents=True, exist_ok=True)
    report.write_text(json.dumps(evidence, indent=2) + '\n')
    return int(any(r['exit_code'] or r['failures'] for r in evidence['runs']))


if __name__ == '__main__':
    sys.exit(main())
