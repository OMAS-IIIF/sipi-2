"""Create a source release for both Linux architectures from one clean Git commit.

Does not commit, tag, publish, or export private/untracked/ignored working files.
Docker/Conan fetch sources if needed; built binaries are excluded from the archive.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile

import docker_image
from version import ROOT, read_version


def sha256(path):
    """Hash a potentially large source archive without loading it into memory."""
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def validate_export(directory):
    """Reject incomplete dependency exports and binary-bearing Conan selections."""
    directory = Path(directory)
    metadata = json.loads((directory/'dependencies.json').read_text())
    dependencies = metadata['dependencies']
    hosts = {d['reference'] for d in dependencies if 'host' in d['contexts']}
    if not hosts or any(not d['sources_included'] for d in dependencies if 'host' in d['contexts']):
        raise ValueError('Source export does not include all host library sources')
    recipes = json.loads((directory/'recipes.json').read_text())['Local Cache']
    references = set()
    for name, item in recipes.items():
        for revision, data in item['revisions'].items():
            if data.get('packages'):
                raise ValueError('Binary packages must not enter a source release')
            references.add(name+'#'+revision)
    if references != {d['reference'] for d in dependencies}:
        raise ValueError('Recipe/source inventory mismatch')
    notices = json.loads((directory/'licenses/inventory.json').read_text())
    if hosts != {c['reference'] for c in notices['components'].values()}:
        raise ValueError('Source and runtime license graphs differ')
    if not (directory/'conan-sources.tgz').is_file() or not (directory/'conan.lock').is_file():
        raise ValueError('Missing Conan archive or resolved lockfile')
    return metadata


def verify_image_report(path, identity, version, exports):
    """Optionally bind source inventories to previously verified local image IDs.

    Uses image IDs, not mutable verification tags. Images must remain available
    locally. Publication's final multi-platform digest still needs release records.
    """
    report = json.loads(Path(path).read_text())
    if (report.get('status') != 'passed' or report.get('dirty') or
            report.get('revision') != identity['revision'] or report.get('version') != version):
        raise ValueError('Image report must describe this clean version/commit and passed tests')
    if {p['platform'] for p in report['platforms']} != set(docker_image.PLATFORMS):
        raise ValueError('Image report must include both platforms')
    for platform in report['platforms']:
        if platform['smoke'] != 'passed':
            raise ValueError('Image smoke test did not pass')
        image_id = docker_image.inspect_verified_image(
            platform['image_id'], platform['platform'], docker_image.image_labels(version, identity))
        if image_id != platform['image_id']:
            raise ValueError('Image report must use immutable image IDs')
        raw = subprocess.check_output(['docker', 'run', '--rm', '--platform', platform['platform'],
              '--entrypoint', 'cat', platform['image_id'], '/opt/sipi/licenses/inventory.json'], text=True)
        actual = json.loads(raw)['components']
        expected = json.loads((exports/platform['platform'].split('/')[1]/'licenses/inventory.json').read_text())['components']
        # Includes recipe revisions, binary IDs/revisions, source URLs and licenses.
        if actual != expected:
            raise ValueError(f'Source export differs from verified image: {platform["platform"]}')
    return report


def package(output_root, jobs=4, image_report=None):
    """Build and atomically expose a new source release directory on success.

    A failed run keeps no misleading final archive. Output directories are unique
    per version/revision and existing releases are never overwritten. Dependencies
    are exported with a fixed Git tar input on both architectures.
    """
    identity = docker_image.source_identity()
    if identity['dirty']:
        raise ValueError('Source release requires a clean commit, including untracked files')
    version = read_version()
    output_root = Path(output_root).resolve()
    output_root.mkdir(parents=True, exist_ok=True)
    destination = output_root/f'{version}-{identity["revision"][:12]}'
    if destination.exists():
        raise FileExistsError(f'Release output already exists: {destination}')
    with tempfile.TemporaryDirectory(prefix='.source-package-', dir=output_root) as temporary:
        work = Path(temporary)
        bundle = work/f'sipi-2-{version}-source'
        bundle.mkdir()
        project = bundle/'sipi-2'
        project.mkdir()
        git_archive = work/'git-source.tar'
        subprocess.run(['git', 'archive', '--format=tar', '--output='+str(git_archive),
                        identity['revision']], cwd=ROOT, check=True)
        with tarfile.open(git_archive) as archive:
            archive.extractall(project, filter='data')
        exports = bundle/'dependencies'
        exports.mkdir()
        metadata = {}
        for platform in docker_image.PLATFORMS:
            docker_image.require_unchanged(identity)
            name = platform.split('/')[1]
            with git_archive.open('rb') as source:
                subprocess.run(['docker', 'buildx', 'build', '--platform', platform,
                    '--build-arg', f'BUILD_JOBS={jobs}', '--target', 'source-export',
                    '--output', f'type=local,dest={exports/name}', '-'],
                    cwd=ROOT, stdin=source, check=True)
            metadata[platform] = validate_export(exports/name)
        report = verify_image_report(image_report, identity, version, exports) if image_report else None
        docker_image.require_unchanged(identity)
        shutil.copyfile(project/'docs/source-package.md', bundle/'README.md')
        manifest = {'version': version, **identity, 'platforms': metadata,
                    'image_report_checked': report is not None,
                    'image_report': report,
                    'scope': 'SIPI-2 Git snapshot plus Conan library sources/recipes; no compiler or OS source bundle',
                    'files': {p.relative_to(bundle).as_posix(): sha256(p)
                              for p in sorted(bundle.rglob('*')) if p.is_file()}}
        (bundle/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
        result = work/'result'
        result.mkdir()
        archive_path = result/f'{bundle.name}.tar.gz'
        with tarfile.open(archive_path, 'w:gz') as archive:
            archive.add(bundle, arcname=bundle.name)
        (result/'SHA256SUMS').write_text(f'{sha256(archive_path)}  {archive_path.name}\n')
        shutil.copyfile(bundle/'manifest.json', result/'manifest.json')
        docker_image.require_unchanged(identity)
        result.rename(destination)
    print(f'Source release: {destination}', flush=True)
    return destination


def main():
    """Read version from CMake and create files only; no registry/GitHub writes."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT/'build/source-releases')
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--image-report', type=Path)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    package(args.output, args.jobs, args.image_report)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        raise SystemExit(f'Source packaging failed: {error}')
