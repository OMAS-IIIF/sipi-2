"""Export exact Conan recipes and available sources, never package binaries.

Runs in the Docker source-export stage after download_source=True. Host library
sources are mandatory. Build-tool recipes are included; build-tool sources may
be absent because general-purpose tools are prerequisites, not runtime libraries.
"""
import argparse
import json
from pathlib import Path
import shutil
import subprocess


def export_sources(graph_path, output):
    """Save a restorable Conan cache archive and a path-free dependency inventory.

    Args:
        graph_path: Installed Conan JSON graph for the current Linux build.
        output: New directory receiving sources, lockfile and runtime notices.

    Raises:
        ValueError: A host dependency has no prepared source or a graph uses
            unsupported editable/python-require recipes.
        CalledProcessError: Conan cannot resolve or export the pinned recipes.
    """
    graph = json.loads(Path(graph_path).read_text())['graph']['nodes']
    output = Path(output)
    output.mkdir(parents=True, exist_ok=False)
    selected = {}
    for node in graph.values():
        if not node.get('name'):
            continue
        if node.get('python_requires') or node.get('recipe') != 'Cache':
            raise ValueError(f'Unsupported recipe provenance: {node["ref"]}')
        reference = node['ref']
        entry = selected.setdefault(reference, {
            'reference': reference, 'contexts': [], 'builds': [],
            'license': node.get('license'),
            'source_archives': node.get('conandata', {}).get('sources', {}).get(node['version'], {}),
        })
        if node['context'] not in entry['contexts']:
            entry['contexts'].append(node['context'])
        configuration = {k: node.get(k) for k in ('context', 'settings', 'options', 'package_id', 'prev')}
        if configuration not in entry['builds']:
            entry['builds'].append(configuration)
    if not selected:
        raise ValueError('Empty dependency graph')
    package_list = {'Local Cache': {}}
    for reference, entry in sorted(selected.items()):
        name, revision = reference.split('#')
        package_list['Local Cache'].setdefault(name, {'revisions': {}})['revisions'][revision] = {}
        source = subprocess.run(['conan', 'cache', 'path', reference, '--folder', 'source'],
                                capture_output=True, text=True)
        entry['sources_included'] = source.returncode == 0 and any(
            p.is_file() for p in Path(source.stdout.strip()).rglob('*'))
        if 'host' in entry['contexts'] and not entry['sources_included']:
            raise ValueError(f'Host source missing: {reference}')
    # Recipe-only selection includes e/es/s (recipes, exported patches, sources),
    # with no package IDs. Conan manages its own restore format and cache metadata.
    listing = output/'recipes.json'
    listing.write_text(json.dumps(package_list, indent=2) + '\n')
    subprocess.run(['conan', 'cache', 'save', '--list', str(listing),
                    '--file', str(output/'conan-sources.tgz')], check=True)
    (output/'dependencies.json').write_text(json.dumps({
        'conan_version': subprocess.check_output(['conan', '--version'], text=True).strip(),
        'scope': 'All host library sources and all graph recipes; available build-tool sources; no binaries',
        'dependencies': list(selected.values()),
    }, indent=2) + '\n')
    shutil.copyfile('/tmp/docker.lock', output/'conan.lock')
    shutil.copytree('/out/licenses', output/'licenses')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('graph', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    export_sources(args.graph, args.output)
