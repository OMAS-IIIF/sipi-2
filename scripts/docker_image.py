"""Build and smoke-test runtime images; publish only after both architectures pass.

Local/test actions never push. Publish uses Docker's existing login/builder and
pushes version plus latest in one multi-platform build. No credentials are read
or logged by this script. Verification tags are retained locally for inspection.
"""
import argparse
import os
import re
import subprocess
import sys
from version import ROOT, read_version

PLATFORMS = ('linux/amd64', 'linux/arm64')


def run(command, **kwargs):
    """Run one visible argv command, propagating any failure before the next phase."""
    print('+ ' + ' '.join(command), flush=True)
    subprocess.run(command, cwd=ROOT, check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('local', 'test', 'publish'))
    parser.add_argument('--image', required=True, help='Repository without tag (e.g. lrosenth/sipi-2)')
    parser.add_argument('--platforms', default=','.join(PLATFORMS))
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    # Keep repository/tag separation explicit; permit a registry port.
    if not re.fullmatch(r'[a-z0-9][a-z0-9.:-]*(?:/[a-z0-9][a-z0-9._-]*)+', args.image) or ':' in args.image.rsplit('/',1)[-1]:
        parser.error('--image must be a lowercase registry/namespace repository without tag or digest')
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    platforms = args.platforms.split(',')
    if len(platforms) != 2 or set(platforms) != set(PLATFORMS):
        parser.error('--platforms must contain linux/amd64 and linux/arm64 exactly once')
    version = read_version()
    if args.action == 'local':
        architecture = subprocess.check_output(['docker','info','--format','{{.Architecture}}'], text=True).strip()
        architecture = {'aarch64':'arm64', 'x86_64':'amd64'}.get(architecture,architecture)
        if 'linux/'+architecture not in PLATFORMS:
            parser.error('Unsupported Docker engine architecture: '+architecture)
        platforms = ['linux/'+architecture]
    base = ['docker','buildx','build','--build-arg',f'BUILD_JOBS={args.jobs}']
    for platform in platforms:
        tag = f'{args.image}:verify-{version}-{platform.split("/")[1]}'
        run(base + ['--platform',platform,'--tag',tag,'--load','.'])
        environment = {**os.environ, 'DOCKER_DEFAULT_PLATFORM':platform}
        run([sys.executable, str(ROOT/'tests/docker_smoke.py'),tag], env=environment)
    if args.action == 'publish':
        # BuildKit reuses the platform builds above. Do not edit source during a release.
        run(base + ['--platform',','.join(platforms),
                    '--tag',f'{args.image}:{version}','--tag',f'{args.image}:latest','--push','.'])
    else:
        print('Container validation passed; nothing published.', flush=True)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        sys.exit(f'Docker workflow failed: {error}')
