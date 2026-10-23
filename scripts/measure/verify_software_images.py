"""Compare same-frame CPU/GPU software images; this does not validate resolve/merge.

Use a fresh prefix for each capture. --pixels must match the configured render extent.
Only GPU files are compared across repeats; the CPU reference can differ by floating point ULPs.

THE CAPTURE MANIFEST (added with the M4 Task 3 closeout hardening): a hardened capture writes
`<prefix>-capture.json` LAST, naming every file it produced and its byte count. When the manifest is present this
verifier checks it against the files on disk (so a mixed/stale prefix is refused); `--require-manifest` additionally
demands one, which is what the acceptance evidence captures use.
"""
import argparse
import array
import hashlib
import json
import tempfile
from pathlib import Path

IMAGES = ('depth', 'id', 'cpu-depth', 'cpu-id')


def image_path(prefix, name):
    return Path(str(prefix) + '-' + name + '.u32')


def read_manifest(prefix):
    """The capture manifest, or None for a legacy capture that predates it."""
    path = Path(str(prefix) + '-capture.json')
    if not path.exists():
        return None
    try:
        data = json.loads(path.read_text(encoding='utf-8'))
    except (OSError, ValueError) as error:
        raise ValueError(f'capture manifest is not valid JSON: {error}') from error
    if not isinstance(data, dict) or data.get('complete') is not True or data.get('schema') != 1:
        raise ValueError('capture manifest does not declare a complete schema-1 capture')
    entries = data.get('files')
    if not isinstance(entries, list):
        raise ValueError('capture manifest has no file list')
    files = {}
    for entry in entries:
        if not isinstance(entry, dict) or not isinstance(entry.get('suffix'), str) or not isinstance(entry.get('bytes'), int):
            raise ValueError('capture manifest has a malformed file entry')
        files[entry['suffix']] = entry['bytes']
    for name in IMAGES:
        suffix = '-' + name + '.u32'
        if suffix not in files:
            raise ValueError(f'capture manifest does not list {suffix}')
    for suffix, expected in files.items():
        path = Path(str(prefix) + suffix)
        if not path.exists() or path.stat().st_size != expected:
            raise ValueError(f'capture manifest and disk disagree about {suffix}')
    return files


def verify(prefix, pixels, max_ulp, require_manifest=False):
    manifest = read_manifest(prefix)
    if manifest is None and require_manifest:
        raise ValueError('capture has no completeness manifest (stale or partial prefix)')
    images = {}
    hashes = {}
    for name in IMAGES:
        path = image_path(prefix, name)
        if not path.exists():
            raise ValueError(f'{name}: missing image file')
        data = path.read_bytes()
        if len(data) != pixels * 4:
            raise ValueError(f'{name}: unexpected image byte count')
        images[name] = array.array('I', data)
        hashes[name] = hashlib.sha256(data).hexdigest()
    if manifest is not None:
        for name in IMAGES:
            if manifest['-' + name + '.u32'] != pixels * 4:
                raise ValueError(f'capture manifest byte count for {name} does not match the configured extent')
    covered = sum(bool(i & 0x80000000) for i in images['id'])
    if not covered:
        raise ValueError('empty GPU raster cannot establish correctness')
    if images['id'] != images['cpu-id']:
        raise ValueError('CPU/GPU coverage or primitive IDs differ')
    max_difference = 0
    for i, (gpu, cpu, identifier) in enumerate(zip(images['depth'], images['cpu-depth'], images['id'])):
        if not identifier:
            if gpu != 0xffffffff or cpu != 0xffffffff:
                raise ValueError(f'pixel {i}: invalid empty encoding')
        else:
            if gpu > 0x3f800000 or cpu > 0x3f800000:
                raise ValueError(f'pixel {i}: invalid normalized depth')
            max_difference = max(max_difference, abs(gpu - cpu))
    if max_difference > max_ulp:
        raise ValueError(f'depth differs by {max_difference} ULP, budget {max_ulp}')
    return covered, max_difference, hashes


def _write_manifest(prefix, extra=None):
    files = [{'suffix': '-' + name + '.u32', 'bytes': image_path(prefix, name).stat().st_size} for name in IMAGES]
    for suffix in extra or ():
        files.append({'suffix': suffix, 'bytes': Path(str(prefix) + suffix).stat().st_size})
    Path(str(prefix) + '-capture.json').write_text(json.dumps({'complete': True, 'schema': 1, 'slot': 0, 'files': files}) + '\n',
                                                   encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('prefix', type=Path, nargs='?')
    parser.add_argument('--pixels', type=int)
    parser.add_argument('--self-test', action='store_true')
    parser.add_argument('--max-depth-ulp', type=int, default=0)
    parser.add_argument('--compare', type=Path)
    parser.add_argument('--require-manifest', action='store_true',
                        help='refuse a prefix without the capture completeness manifest')
    args = parser.parse_args()
    if args.self_test:
        scratch = Path(__file__).resolve().parents[2] / 'tmp'
        scratch.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=scratch) as folder:
            prefix = Path(folder) / 'capture'
            good = {'depth': [0x3f000000, 0xffffffff], 'cpu-depth': [0x3f000002, 0xffffffff],
                    'id': [0x80000000, 0], 'cpu-id': [0x80000000, 0]}
            def write():
                for name, values in good.items():
                    image_path(prefix, name).write_bytes(array.array('I', values).tobytes())
            write()
            _write_manifest(prefix)
            assert verify(prefix, 2, 2, require_manifest=True)[0] == 1
            # A legacy capture without the manifest still verifies, but NOT when the manifest is required.
            Path(str(prefix) + '-capture.json').unlink()
            assert verify(prefix, 2, 2)[0] == 1
            try:
                verify(prefix, 2, 2, require_manifest=True)
            except ValueError:
                pass
            else:
                raise AssertionError('accepted a capture without the required manifest')
            # A manifest that disagrees with the files on disk is refused (stale/mixed prefix).
            _write_manifest(prefix)
            image_path(prefix, 'id').write_bytes(array.array('I', [0x80000000, 0]).tobytes() + b'\0\0\0\0')
            for check in (lambda: verify(prefix, 2, 2, require_manifest=True),):
                try:
                    check()
                except ValueError:
                    pass
                else:
                    raise AssertionError('accepted a manifest that disagrees with the images')
            write()
            # ... and a truncated manifest is refused too.
            Path(str(prefix) + '-capture.json').write_text('{"complete":false}', encoding='utf-8')
            try:
                verify(prefix, 2, 2, require_manifest=True)
            except ValueError:
                pass
            else:
                raise AssertionError('accepted an incomplete manifest')
            _write_manifest(prefix)
            for name, values in [('id', [0, 0]), ('depth', [0x7fc00000, 0xffffffff]),
                                 ('cpu-id', [0x80000001, 0]), ('cpu-depth', [0x3f100000, 0xffffffff])]:
                write()
                image_path(prefix, name).write_bytes(array.array('I', values).tobytes())
                _write_manifest(prefix)
                try:
                    verify(prefix, 2, 2, require_manifest=True)
                except ValueError:
                    pass
                else:
                    raise AssertionError(f'accepted corrupt {name}')
        print('PASS: raw software image verifier positive and corruption/manifest controls')
        return
    if args.prefix is None or args.pixels is None:
        parser.error('prefix and --pixels are required')
    if args.pixels <= 0 or args.max_depth_ulp < 0:
        parser.error('pixels must be positive and ULP budget nonnegative')
    covered, difference, hashes = verify(args.prefix, args.pixels, args.max_depth_ulp, args.require_manifest)
    if args.compare:
        _, _, other = verify(args.compare, args.pixels, args.max_depth_ulp, args.require_manifest)
        if any(hashes[name] != other[name] for name in ('depth', 'id')):
            raise ValueError('GPU image hashes differ between captures')
    manifest = 'with manifest' if read_manifest(args.prefix) is not None else 'legacy (no manifest)'
    print(f'PASS: {covered} covered pixels; maximum depth difference {difference} ULP; {manifest}; {hashes}')


if __name__ == '__main__':
    main()
