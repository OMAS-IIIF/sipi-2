"""Compare concurrent TIFF/Grok rendering with serial output and verify cache fills.

No timing thresholds: the C++ worker test proves overlap using held callbacks;
this test stresses real codecs, shared metadata and result publication.
"""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import shutil
import sys
import tempfile
from cache_http import server, get, success, rows


def main():
    executable, fixtures = (Path(p).resolve() for p in sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix='sipi-parallel-') as temporary:
        root = Path(temporary)
        images = root / 'images'
        images.mkdir()
        for name in ('tiled.tif', 'alpha.tif', 'gray.tif', 'source.jp2', 'source.j2k'):
            shutil.copyfile(fixtures / name, images / name)
        (root / 'policy.lua').write_text("""
function preflight(request)
  if request.headers.authorization == 'denied' then return {decision='deny'} end
  return {decision='allow'}
end
""")
        paths = [f'/{name}/full/max/{angle}/default.{ext}'
                 for name in ('tiled.tif', 'alpha.tif', 'gray.tif', 'source.jp2', 'source.j2k')
                 for angle in ('0', '!90') for ext in ('png', 'jpg')]
        with server(executable, root, ['--iiif-workers=1'], cache=None) as (port, _):
            expected = {path: get(port, path)[2] for path in paths}
            assert all(expected.values())
        # No result cache: every call decodes/encodes; metadata is shared across workers.
        with server(executable, root, cache=None, variables={'SIPI_IIIF_WORKERS': '4'}) as (port, log):
            def verify(path):
                status, _, body = get(port, path)
                assert status == 200 and body == expected[path], path
            with ThreadPoolExecutor(max_workers=12) as clients:
                list(clients.map(verify, paths * 4))
        assert 'IIIF processing pool: 4 workers' in log[0]
        # A cold shared cache: repeated/aliased requests must each authorize but
        # generate each distinct result exactly once, including 0/360 aliases.
        with server(executable, root, ['--iiif-workers=4']) as (port, log):
            def cached(path):
                status, _, body = get(port, path)
                original = path.replace('/360/', '/0/')
                assert status == 200 and body == expected[original], path
            work = [alias for path in paths for alias in
                    (path, path, path.replace('/0/', '/360/'), path)]
            with ThreadPoolExecutor(max_workers=12) as clients:
                list(clients.map(cached, work))
                denied = list(clients.map(lambda path: get(port, path, authorization='denied')[0], paths))
            assert all(status == 403 for status in denied)
            assert len(rows(root)) == len(paths)
        assert log[0].count('IIIF result cache miss') == len(paths), log[0]
        assert log[0].count('IIIF result cache stored') == len(paths), log[0]
        assert 'publication failed' not in log[0] and 'lookup failed' not in log[0]
        # Publication rejected by capacity: concurrent callers still receive valid
        # output, retries don't leave stuck per-key locks or persistent entries.
        with server(executable, root, ['--iiif-workers=4', '--cache-max-bytes=1'], cache='tiny') as (port, _):
            with ThreadPoolExecutor(max_workers=8) as clients:
                results = list(clients.map(lambda _: success(get(port, paths[0])), range(16)))
            assert all(body == expected[paths[0]] for body in results)
            assert not rows(root, 'tiny')
    print('Concurrent codecs, shared metadata, authorized cache fills and retry checks passed')


if __name__ == '__main__':
    main()
