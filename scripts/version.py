"""Read or increment the sole CMake project version, without Git side effects.

Usage: python3 scripts/version.py show|patch|minor|major
Only the project declaration is changed. Writes use an atomic sibling replacement.
Concurrent edits/version bumps are not supported; release commands run serially.
"""
from pathlib import Path
import argparse
import os
import re
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PATTERN = re.compile(r'(?m)^(project\(sipi-2\s+VERSION\s+)(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(\s+LANGUAGES\s+CXX\))$')


def version_match(text):
    """Require exactly one unambiguous supported CMake project declaration."""
    matches = list(PATTERN.finditer(text))
    if len(matches) != 1:
        raise ValueError('Expected exactly one project(sipi-2 VERSION major.minor.patch LANGUAGES CXX)')
    return matches[0]


def read_version(path=ROOT/'CMakeLists.txt'):
    """Return the three decimal components as a string; never modify the file."""
    match = version_match(path.read_text())
    return '.'.join(match.group(i) for i in (2, 3, 4))


def bump(path, part):
    """Increment one component and reset lower components; preserve other content/mode."""
    text = path.read_text()
    match = version_match(text)
    parts = [int(match.group(i)) for i in (2, 3, 4)]
    index = {'major': 0, 'minor': 1, 'patch': 2}[part]
    parts[index] += 1
    parts[index+1:] = [0] * (2-index)
    new = '.'.join(map(str, parts))
    updated = text[:match.start(2)] + new + text[match.end(4):]
    name = None
    try:
        with tempfile.NamedTemporaryFile(mode='w', dir=path.parent, delete=False) as output:
            name = Path(output.name)
            output.write(updated)
            output.flush()
            os.fsync(output.fileno())
        name.chmod(path.stat().st_mode)
        name.replace(path)
    finally:
        if name is not None:
            name.unlink(missing_ok=True)
    return new


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('show', 'patch', 'minor', 'major'))
    args = parser.parse_args()
    try:
        old = read_version()
        print(old if args.action == 'show' else f'{old} -> {bump(ROOT/"CMakeLists.txt", args.action)}')
    except (OSError, ValueError) as error:
        parser.exit(1, f'Version update failed: {error}\n')


if __name__ == '__main__':
    main()
