"""Assemble runtime notices from the exact installed Conan host graph.

Input is `conan install --format=json`, not `graph info` (which lacks package
folders). Output must be a new directory. All host packages are retained,
including header/static libraries; this is a conservative inventory, not a
linker-level proof that every object is used. No network access is performed.
"""
from pathlib import Path
import argparse
import hashlib
import json
import shutil


def collect(graph_path, output, root):
    """Copy license bytes and pinned supplements; fail on absent notices.

    Args:
        graph_path: JSON output from the successful Conan install for this build.
        output: New output directory; partial output after failure is not valid.
        root: Repository root containing project terms and supplemental notices.

    Returns:
        The inventory written to inventory.json, without local cache paths.

    Raises:
        ValueError: Missing licenses, ambiguous package names, or stale supplements.
        FileExistsError: The output already exists; old evidence is never overwritten.
    """
    graph = json.loads(Path(graph_path).read_text())['graph']['nodes']
    nodes = [n for n in graph.values() if n.get('context') == 'host' and n.get('name')]
    if not nodes:
        raise ValueError('No host packages in installed Conan graph')
    output = Path(output)
    output.mkdir(parents=True, exist_ok=False)
    root = Path(root)
    components = {}
    for node in sorted(nodes, key=lambda n: n['name']):
        name = node['name']
        if name in components or '/' in name or name in ('.', '..'):
            raise ValueError(f'Ambiguous package name: {name}')
        folder = node.get('package_folder')
        source = Path(folder)/'licenses' if folder else None
        files = sorted(p for p in source.rglob('*') if p.is_file()) if source else []
        if not files or not node.get('license'):
            raise ValueError(f'Missing license metadata/files for {node["ref"]}')
        target = output/'dependencies'/name
        for file in files:
            dest = target/file.relative_to(source)
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(file, dest)
        components[name] = {
            'reference': node['ref'], 'package_id': node.get('package_id'),
            'package_revision': node.get('prev'), 'declared_license': node['license'],
            'homepage': node.get('homepage'),
            'source_archives': node.get('conandata', {}).get('sources', {}).get(node['version'], {}),
        }
    supplements = root/'third_party/notices'
    manifest = json.loads((supplements/'manifest.json').read_text())
    for entry in manifest['files']:
        name = entry['reference'].split('/')[0]
        # Some dependencies (for example s2n) exist only on particular platforms.
        if name not in components:
            continue
        if components[name]['reference'] != entry['reference']:
            raise ValueError(f'Review supplemental notices for changed dependency: {name}')
        relative = Path(entry['file'])
        if relative.is_absolute() or '..' in relative.parts:
            raise ValueError('Supplement must be repository-relative')
        data = (supplements/relative).read_bytes()
        if hashlib.sha256(data).hexdigest() != entry['sha256']:
            raise ValueError(f'Supplement checksum mismatch: {relative}')
        target = output/'supplemental'/relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    shutil.copyfile(supplements/'manifest.json', output/'supplemental-manifest.json')
    for name in ('LICENSE', 'LICENSING.md', 'THIRD_PARTY_NOTICES.md'):
        shutil.copyfile(root/name, output/name)
    shutil.copytree(root/'LICENSES', output/'LICENSES')
    shutil.copytree(root/'third_party/compact-icc', output/'compact-icc',
                    ignore=shutil.ignore_patterns('*.icc'))
    inventory = {'scope': 'Conan host dependency superset; OS packages listed separately',
                 'components': components,
                 'files': {p.relative_to(output).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                           for p in sorted(output.rglob('*')) if p.is_file()}}
    (output/'inventory.json').write_text(json.dumps(inventory, indent=2) + '\n')
    return inventory


def main():
    """Collect notices without installing dependencies or modifying the source tree."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('graph', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    inventory = collect(args.graph, args.output, args.root)
    print(f'Collected notices for {len(inventory["components"])} host packages')


if __name__ == '__main__':
    main()
