"""Build/test images with source metadata; only explicit publish pushes registry tags.

Publish requires a clean committed tree and builds from a fixed Git archive.
Local/test may use dirty development sources, labelled explicitly as such.
Reports and Buildx metadata stay under ignored build/releases; no Git tag is made.
"""
import argparse
from contextlib import nullcontext
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from version import ROOT, read_version

PLATFORMS = ('linux/amd64', 'linux/arm64')
SOURCE_URL = 'https://github.com/OMAS-IIIF/sipi-2'


def run(command, **kwargs):
    """Execute one visible argv, stopping the workflow on failure."""
    print('+ ' + ' '.join(command), flush=True)
    subprocess.run(command, cwd=ROOT, check=True, **kwargs)


def source_identity():
    """Read committed identity and staged/unstaged/untracked state; ignored builds excluded."""
    revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    if not re.fullmatch(r'[0-9a-f]{40,64}', revision):
        raise ValueError('Expected a full Git commit ID')
    dirty = bool(subprocess.check_output(
        ['git', 'status', '--porcelain', '--untracked-files=all'], cwd=ROOT, text=True).strip())
    return {'revision': revision, 'dirty': dirty}


def require_unchanged(identity):
    """Reject revision/state changes; a dirty development build is not a reproducible snapshot."""
    if source_identity() != identity:
        raise ValueError('Source revision/state changed during verification; rerun the workflow')


def image_labels(version, identity):
    """Keep development output distinct from an image of exactly the recorded commit."""
    return {
        'org.opencontainers.image.title': 'SIPI-2',
        'org.opencontainers.image.source': SOURCE_URL,
        'org.opencontainers.image.version': version,
        'org.opencontainers.image.revision': identity['revision'] + ('-dirty' if identity['dirty'] else ''),
        'org.omas-iiif.sipi.source-state': 'dirty' if identity['dirty'] else 'clean',
    }


def inspect_verified_image(tag, platform, labels):
    """Fail before smoke/publication if architecture or supplied provenance was lost."""
    image = json.loads(subprocess.check_output(['docker', 'image', 'inspect', tag], text=True))[0]
    if image['Os'] + '/' + image['Architecture'] != platform:
        raise ValueError('Built image architecture differs from the requested platform')
    actual = image['Config'].get('Labels') or {}
    if any(actual.get(key) != value for key, value in labels.items()):
        raise ValueError('Built image provenance labels do not match the source snapshot')
    return image['Id']


def build_metadata(path):
    """Require Buildx's result digest; retain full metadata alongside the compact report."""
    metadata = json.loads(path.read_text())
    digest = metadata.get('containerimage.digest', '')
    if not re.fullmatch(r'sha256:[0-9a-f]{64}', digest):
        raise ValueError('Buildx did not return a valid image digest; inspect build metadata')
    return digest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('local', 'test', 'publish'))
    parser.add_argument('--image', required=True, help='Repository without tag (e.g. lrosenth/sipi-2)')
    parser.add_argument('--platforms', default=','.join(PLATFORMS))
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    if not re.fullmatch(r'[a-z0-9][a-z0-9.:-]*(?:/[a-z0-9][a-z0-9._-]*)+', args.image) or ':' in args.image.rsplit('/', 1)[-1]:
        parser.error('--image must be a lowercase registry/namespace repository without tag or digest')
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    platforms = args.platforms.split(',')
    if len(platforms) != 2 or set(platforms) != set(PLATFORMS):
        parser.error('--platforms must contain linux/amd64 and linux/arm64 exactly once')
    identity = source_identity()
    if args.action == 'publish' and identity['dirty']:
        raise ValueError('Publication requires a clean committed tree, including untracked files')
    version = read_version()
    labels = image_labels(version, identity)
    if args.action == 'local':
        architecture = subprocess.check_output(['docker', 'info', '--format', '{{.Architecture}}'], text=True).strip()
        architecture = {'aarch64': 'arm64', 'x86_64': 'amd64'}.get(architecture, architecture)
        if 'linux/' + architecture not in PLATFORMS:
            parser.error('Unsupported Docker engine architecture: ' + architecture)
        platforms = ['linux/' + architecture]
    output = ROOT / 'build/releases'
    output.mkdir(parents=True, exist_ok=True)
    report_dir = Path(tempfile.mkdtemp(prefix=f'{version}-{args.action}-', dir=output))
    report = {'version': version, 'source': SOURCE_URL, **identity, 'action': args.action,
              'started_at': datetime.now(timezone.utc).isoformat(), 'status': 'running',
              'platforms': [], 'publication': 'not attempted'}
    print('Release report: ' + str(report_dir / 'report.json'), flush=True)
    base = ['docker', 'buildx', 'build', '--build-arg', f'BUILD_JOBS={args.jobs}']
    for key, value in labels.items():
        base += ['--label', f'{key}={value}']
    try:
        # Only committed files enter a publication context. Reopening the same tar
        # for every build avoids mixed platform sources if the worktree changes.
        with tempfile.TemporaryDirectory(prefix='sipi-release-source-') as temporary:
            archive = Path(temporary) / 'source.tar'
            if args.action == 'publish':
                run(['git', 'archive', '--format=tar', '--output=' + str(archive), identity['revision']])
            def build(options):
                require_unchanged(identity)
                with archive.open('rb') if args.action == 'publish' else nullcontext(None) as source:
                    run(base + options + (['-'] if source else ['.']), stdin=source)
            for platform in platforms:
                name = platform.split('/')[1]
                tag = f'{args.image}:verify-{version}-{name}'
                metadata = report_dir / (name + '.json')
                build(['--platform', platform, '--tag', tag, '--metadata-file', str(metadata), '--load'])
                item = {'platform': platform, 'tag': tag, 'digest': build_metadata(metadata),
                        'image_id': inspect_verified_image(tag, platform, labels), 'smoke': 'pending'}
                report['platforms'].append(item)
                run([sys.executable, str(ROOT / 'tests/docker_smoke.py'), tag],
                    env={**os.environ, 'DOCKER_DEFAULT_PLATFORM': platform})
                item['smoke'] = 'passed'
                require_unchanged(identity)
            if args.action == 'publish':
                metadata = report_dir / 'published.json'
                report['publication'] = 'attempting; consult metadata/registry if interrupted'
                (report_dir / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
                build(['--platform', ','.join(platforms), '--tag', f'{args.image}:{version}',
                       '--tag', f'{args.image}:latest', '--metadata-file', str(metadata), '--push'])
                report['published_digest'] = build_metadata(metadata)
                report['publication'] = 'pushed'
            else:
                print('Container validation passed; nothing published.', flush=True)
            report['status'] = 'passed'
    except BaseException:
        report['status'] = 'failed or interrupted; inspect command output'
        raise
    finally:
        report['finished_at'] = datetime.now(timezone.utc).isoformat()
        (report_dir / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        sys.exit(f'Docker workflow failed: {error}')
