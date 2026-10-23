"""Compare same-frame CPU/GPU software images; this does not validate resolve/merge.

Use a fresh prefix for each capture. --pixels must match the configured render extent.
Only GPU files are compared across repeats; the CPU reference can differ by floating point ULPs.
"""
import argparse
import array
import hashlib
import tempfile
from pathlib import Path


def verify(prefix, pixels, max_ulp):
    images = {}
    hashes = {}
    for name in ('depth', 'id', 'cpu-depth', 'cpu-id'):
        data = Path(str(prefix) + '-' + name + '.u32').read_bytes()
        if len(data) != pixels * 4:
            raise ValueError(f'{name}: unexpected image byte count')
        images[name] = array.array('I', data)
        hashes[name] = hashlib.sha256(data).hexdigest()
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('prefix', type=Path, nargs='?')
    parser.add_argument('--pixels', type=int)
    parser.add_argument('--self-test', action='store_true')
    parser.add_argument('--max-depth-ulp', type=int, default=0)
    parser.add_argument('--compare', type=Path)
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
                    Path(str(prefix) + '-' + name + '.u32').write_bytes(array.array('I', values).tobytes())
            write()
            assert verify(prefix, 2, 2)[0] == 1
            for name, values in [('id', [0, 0]), ('depth', [0x7fc00000, 0xffffffff]),
                                 ('cpu-id', [0x80000001, 0]), ('cpu-depth', [0x3f100000, 0xffffffff])]:
                write()
                Path(str(prefix) + '-' + name + '.u32').write_bytes(array.array('I', values).tobytes())
                try:
                    verify(prefix, 2, 2)
                except ValueError:
                    pass
                else:
                    raise AssertionError(f'accepted corrupt {name}')
        print('PASS: raw software image verifier positive and corruption controls')
        return
    if args.prefix is None or args.pixels is None:
        parser.error('prefix and --pixels are required')
    if args.pixels <= 0 or args.max_depth_ulp < 0:
        parser.error('pixels must be positive and ULP budget nonnegative')
    covered, difference, hashes = verify(args.prefix, args.pixels, args.max_depth_ulp)
    if args.compare:
        _, _, other = verify(args.compare, args.pixels, args.max_depth_ulp)
        if any(hashes[name] != other[name] for name in ('depth', 'id')):
            raise ValueError('GPU image hashes differ between captures')
    print(f'PASS: {covered} covered pixels; maximum depth difference {difference} ULP; {hashes}')


if __name__ == '__main__':
    main()
