#!/usr/bin/env python3
"""Prepare user-supplied archive TIFFs without modifying or copying originals.

Run the offline C++ LCMS/Grok helper, independently decode output with Pillow
(libjpeg2000/OpenJPEG), check all TIFF pyramid levels, and write an audit manifest.
Requires Pillow only; use the existing external-validator virtual environment.
The manifest stays incomplete if any normalization, encoding or validation fails.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import struct
import subprocess
import sys

import PIL
from PIL import Image, ImageCms, features

# This explicit offline corpus includes a 229 MP map. Keep a finite limit.
Image.MAX_IMAGE_PIXELS = 300_000_000


def sha(data):
    return hashlib.sha256(data).hexdigest()


def file_sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def jp2_header(path):
    """Inspect actual JP2 boxes and main COD/SIZ markers, not Pillow's 8-bit view.

    Pillow can display a 16-bit RGB codestream as RGB8 and does not expose JP2 ICC
    here. Read precision/profile from the file; use Pillow only for decoded pixels.
    This is a bounded fixture inspector, not a general JPEG2000 implementation.
    """
    data = path.read_bytes()
    result = {}
    def boxes(start, end):
        while start < end:
            if end-start < 8:
                raise ValueError('Truncated JP2 box')
            size, kind = struct.unpack_from('>I4s', data, start)
            header = 8
            if size == 1:
                size = struct.unpack_from('>Q', data, start+8)[0]
                header = 16
            elif size == 0:
                size = end-start
            if size < header or start+size > end:
                raise ValueError('Invalid JP2 box length')
            payload = data[start+header:start+size]
            if kind == b'ftyp':
                result['brand'] = payload[:4].decode('ascii')
            elif kind == b'jp2h':
                boxes(start+header, start+size)
            elif kind == b'colr':
                result['colour_method'] = payload[0]
                if payload[0] == 2:
                    result['icc_sha256'] = sha(payload[3:])
                    result['icc_bytes'] = len(payload)-3
                elif payload[0] == 1:
                    result['enumerated_colourspace'] = int.from_bytes(payload[3:7], 'big')
            elif kind == b'jp2c':
                if not payload.startswith(b'\xff\x4f'):
                    raise ValueError('Missing JPEG2000 SOC')
                cursor = 2
                while cursor+4 <= len(payload):
                    marker = payload[cursor:cursor+2]
                    if marker == b'\xff\x90':  # first tile part: stop before entropy data
                        break
                    length = int.from_bytes(payload[cursor+2:cursor+4], 'big')
                    if length < 2 or cursor+2+length > len(payload):
                        raise ValueError('Invalid JPEG2000 marker length')
                    segment = payload[cursor:cursor+2+length]
                    if marker == b'\xff\x51':
                        result['rsiz'] = int.from_bytes(segment[4:6], 'big')
                        count = int.from_bytes(segment[38:40], 'big')
                        result['bits'] = [(segment[40+3*c] & 127)+1 for c in range(count)]
                        result['signed'] = [bool(segment[40+3*c] & 128) for c in range(count)]
                        result['tile_size'] = list(struct.unpack_from('>II', segment, 22))
                    elif marker == b'\xff\x50':
                        result['cap'] = segment[4:].hex()
                    elif marker == b'\xff\x52':
                        result.update(progression=segment[5], layers=int.from_bytes(segment[6:8], 'big'),
                                      mct=segment[8], resolutions=segment[9]+1,
                                      codeblock=[2**(segment[10]+2), 2**(segment[11]+2)],
                                      block_style=segment[12], reversible_5_3=segment[13]==1)
                    cursor += length+2
            start += size
    boxes(0, len(data))
    if 'bits' not in result or 'resolutions' not in result:
        raise ValueError('Missing SIZ/COD')
    return result


def inventory(path):
    """Record technical provenance; do not infer rights or publisher from a name."""
    result = {'file': path.name, 'bytes': path.stat().st_size, 'sha256': file_sha(path)}
    with Image.open(path) as image:
        if image.width * image.height > Image.MAX_IMAGE_PIXELS:
            raise ValueError('Source exceeds 300 MP preparation limit')
        result.update(size=list(image.size), pillow_mode=image.mode, frames=getattr(image, 'n_frames', 1))
        if path.suffix.lower() == '.jp2':
            result['jp2'] = jp2_header(path)
        else:
            result['tiff'] = {name: image.tag_v2.get(tag) for tag, name in (
                (258, 'bits'), (259, 'compression'), (262, 'photometric'), (274, 'orientation'),
                (277, 'samples'), (278, 'rows_per_strip'), (322, 'tile_width'),
                (323, 'tile_height'), (338, 'extra_samples'))}
            profile = image.info.get('icc_profile', b'')
            if profile:
                result['icc_sha256'] = sha(profile)
                result['icc_description'] = ImageCms.getProfileDescription(
                    ImageCms.ImageCmsProfile(io.BytesIO(profile))).strip()
    return result


def verify(directory):
    """Fully decode all outputs, compare full-size pixels and independent 2x means."""
    with Image.open(directory/'reference.tif') as image:
        image.load()
        reference = image.copy()
        profile = image.info['icc_profile']
    if reference.mode not in ('RGB', 'L'):
        raise ValueError('Expected normalized RGB8/Gray8')
    with Image.open(directory/'image.jp2') as image:
        image.load()
        if image.size != reference.size or image.mode != reference.mode or image.tobytes() != reference.tobytes():
            raise ValueError('JP2 full-resolution pixels differ')
    header = jp2_header(directory/'image.jp2')
    if (header['icc_sha256'] != sha(profile) or header['bits'] != [8]*len(reference.getbands())
            or header['signed'] != [False]*len(reference.getbands())
            or header['tile_size'] != [1024,1024] or header['resolutions'] != 6
            or header['mct'] != (1 if reference.mode == 'RGB' else 0) or not header['reversible_5_3']):
        raise ValueError('Wrong JP2 encoding/profile')
    expected = reference
    levels = []
    with Image.open(directory/'pyramid.tif') as image:
        for level in range(image.n_frames):
            image.seek(level)
            image.load()
            if (image.size != expected.size or image.mode != reference.mode
                    or image.tobytes() != expected.tobytes() or image.info.get('icc_profile') != profile):
                raise ValueError(f'TIFF level {level} differs')
            tags = image.tag_v2
            if (tags.get(322), tags.get(323), tags.get(259), tags.get(317), tags.get(254,0)) != (256,256,5,2,1 if level else 0):
                raise ValueError('Wrong TIFF pyramid encoding')
            levels.append(list(image.size))
            expected = expected.reduce(2)  # independent, includes odd-edge rounding
    if max(levels[-1]) > 256 or len(levels) < 2:
        raise ValueError('Incomplete pyramid')
    return {'size': list(reference.size), 'mode': reference.mode, 'pixel_sha256': sha(reference.tobytes()),
            'icc_sha256': sha(profile), 'levels': levels, 'jp2': header,
            'files': {p.name: {'bytes':p.stat().st_size, 'sha256':file_sha(p)}
                      for p in sorted(directory.iterdir()) if p.suffix in ('.tif','.jp2','.icc')}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--helper', type=Path, required=True)
    args = parser.parse_args()
    source, output, helper = (p.resolve() for p in (args.source,args.output,args.helper))
    if output == source or source in output.parents:
        parser.error('Place generated files outside the original directory')
    originals = sorted(p for p in source.iterdir() if p.suffix.lower() in ('.tif','.tiff','.jp2'))
    tiffs = [p for p in originals if p.suffix.lower() in ('.tif','.tiff')]
    if not tiffs or len({p.stem for p in tiffs}) != len(tiffs):
        parser.error('Need TIFF files with distinct stems')
    output.mkdir(parents=True, exist_ok=False)
    manifest = {'complete':False, 'recipe':'archive-pairs-v1',
                'helper_sha256':file_sha(helper), 'orchestrator_sha256':file_sha(Path(__file__)),
                'python':sys.version.split()[0], 'pillow':PIL.__version__,
                'openjpeg':features.version('jpg_2000'),
                'provenance':'User-supplied SALSAH and web samples; external URLs/licences not yet recorded.',
                'originals':[inventory(p) for p in originals], 'prepared':{}}
    def save():
        (output/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    save()
    for path in tiffs:
        directory = output/path.stem
        print(f'Preparing {path.name}', flush=True)
        helper_input = path
        palette_expanded = False
        with Image.open(path) as source_image:
            if source_image.mode == 'P':
                if (source_image.tag_v2.get(258) != (8,) or 'transparency' in source_image.info
                        or any(value % 257 for value in source_image.tag_v2[320])):
                    raise ValueError('Palette preparation requires opaque indices and exactly 8-bit-representable colours')
                staged = output / '_inputs'
                staged.mkdir(exist_ok=True)
                helper_input = staged / path.name
                # Indexed colour is not grayscale. Expand the main IFD, preserve
                # its ICC if present, and regenerate overviews from full pixels.
                with source_image.convert('RGB') as expanded:
                    expanded.save(helper_input, format='TIFF', compression='raw',
                                  icc_profile=source_image.info.get('icc_profile'))
                palette_expanded = True
        process = subprocess.run([str(helper),str(helper_input),str(directory)],capture_output=True,text=True)
        if directory.exists():
            (directory/'preparation.log').write_text(process.stdout+process.stderr)
        process.check_returncode()
        result = verify(directory)
        numeric = process.stdout.splitlines()[0].split()
        result['normalization'] = dict(zip(('width','height','channels','source_bits','source_samples',
                                           'alpha_min','alpha_max','source_tagged'),map(int,numeric)))
        result['palette_expanded'] = palette_expanded
        if palette_expanded:
            result['expanded_input_sha256'] = file_sha(helper_input)
            helper_input.unlink()  # Own reproducible staging file; never an original.
        result['libraries'] = process.stdout.splitlines()[1]
        manifest['prepared'][path.stem] = result
        save()
        print(f'  verified {result["mode"]} {result["size"]}, {len(result["levels"])} TIFF levels; JP2 pixels identical',flush=True)
    for path, record in zip(originals,manifest['originals']):
        if file_sha(path) != record['sha256']:
            raise RuntimeError(f'Original changed during preparation: {path}')
    manifest['originals_unchanged'] = True
    manifest['complete'] = True
    save()
    print(f'Completed: {output / "manifest.json"}')


if __name__ == '__main__':
    main()
