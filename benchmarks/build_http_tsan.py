#!/usr/bin/env python3
"""Build SIPI plus instrumented Drogon/Trantor in an isolated work directory.

Uses pinned Conan sources and Debug dependencies, without replacing cached Conan
binaries. The other dependencies stay uninstrumented; this targets the HTTP queue
handoff reports, not comprehensive instrumentation of every native library.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess


def run(args):
    print('+', ' '.join(str(arg) for arg in args), flush=True)
    subprocess.run([str(arg) for arg in args], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True, help='New isolated build directory')
    parser.add_argument('--build-profile', default='sipi-2-local')
    parser.add_argument('--jobs', type=int, default=6)
    options = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    root = options.output.resolve()
    root.mkdir(parents=True, exist_ok=False)
    dependencies = root / 'dependencies'
    dependencies.mkdir()
    # Direct requirements expose private dependency headers needed to rebuild
    # Trantor/Drogon, unlike the application's transitive-only CMakeDeps graph.
    lock = json.loads((repo / 'conan.lock').read_text())
    references = {ref.split('/')[0]: ref.split('%')[0] for ref in lock['requires']}
    text = (repo / 'conanfile.txt').read_text()
    text = text.replace('[requires]', '[requires]\n' + '\n'.join(
        references[name] for name in ('c-ares', 'jsoncpp', 'zlib')))
    (dependencies / 'conanfile.txt').write_text(text)
    run(['conan', 'install', dependencies, '-pr:h', repo / 'profiles/macos-debug',
         '-pr:b', options.build_profile, '--lockfile', repo / 'conan.lock',
         '-c', 'tools.cmake.cmaketoolchain:user_presets=', '--build=missing'])
    generators = dependencies / 'build/Debug/generators'
    # Match the Jsoncpp_lib alias supplied by Drogon's own Conan recipe.
    # Keep the normal JsonCpp::JsonCpp target available to the application too.
    (generators / 'JsoncppConfig.cmake').write_text(
        'include("${CMAKE_CURRENT_LIST_DIR}/jsoncpp-config.cmake")\n'
        'if(NOT TARGET Jsoncpp_lib)\n'
        '  add_library(Jsoncpp_lib ALIAS JsonCpp::JsonCpp)\n'
        'endif()\n')
    prefix = root / 'prefix'
    flags = '-fsanitize=thread -fno-omit-frame-pointer -Wno-error=unused-result'

    def configure(source, build, extra=()):
        run(['cmake', '-S', source, '-B', build, '-G', 'Ninja',
             '-DCMAKE_BUILD_TYPE=Debug', '-DCMAKE_FIND_PACKAGE_PREFER_CONFIG=ON',
             f'-DCMAKE_PREFIX_PATH={generators}', f'-DCMAKE_MODULE_PATH={generators}',
             f'-DCMAKE_CXX_FLAGS={flags}', f'-DCMAKE_INSTALL_PREFIX={prefix}', *extra])

    for name, settings in (
        ('trantor', ['-DBUILD_SHARED_LIBS=OFF', '-DBUILD_TESTING=OFF', '-DBUILD_C-ARES=ON']),
        ('drogon', ['-DBUILD_SHARED_LIBS=OFF', '-DBUILD_TESTING=OFF', '-DUSE_SUBMODULE=OFF',
                    '-DBUILD_CTL=OFF', '-DBUILD_EXAMPLES=OFF', '-DBUILD_ORM=OFF',
                    '-DBUILD_BROTLI=OFF', '-DBUILD_YAML_CONFIG=OFF'])):
        source = Path(subprocess.check_output(
            ['conan', 'cache', 'path', references[name], '--folder=source'], text=True).strip()) / 'src'
        configure(source, root / name, settings)
        run(['cmake', '--build', root / name, '-j', options.jobs])
        run(['cmake', '--install', root / name])
        # Modify only this isolated generation, not the project's presets or the
        # Conan cache. Both includes and archive location now use our installation.
        changed = 0
        for data in generators.glob('*-debug-*-data.cmake'):
            value, count = re.subn(rf'set\({name}_PACKAGE_FOLDER_DEBUG "[^"]+"\)',
                                  f'set({name}_PACKAGE_FOLDER_DEBUG "{prefix}")', data.read_text())
            if count:
                data.write_text(value)
                changed += count
        if changed != 1:
            raise RuntimeError(f'Expected exactly one generated package path for {name}, found {changed}')
    configure(repo, root / 'app')
    run(['cmake', '--build', root / 'app', '--target', 'sipi-2', 'iiif-worker-tests', '-j', options.jobs])
    print(f'Instrumented HTTP binary: {root / "app/sipi-2"}')


if __name__ == '__main__':
    main()
