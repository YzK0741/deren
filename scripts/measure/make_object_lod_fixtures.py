"""Generate small runtime fixtures with the accepted standalone converter.

The checked-in wire payloads, source geometry and cooked GLBs need no converter
binary at test time. --verify checks the manifest using only Python's stdlib.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import tempfile

from make_subtilis_converter_fixtures import glb_bytes, grid_geometry


def sha(data):
    return hashlib.sha256(data).hexdigest()


def geometry(width, seam=False):
    positions, normals, uv, indices = grid_geometry(width)
    positions = [(x / 2, y / 2, .02 * math.sin((x / 2 * width) * .2)
                  * math.cos((y / 2 * width) * .2)) for x, y, _ in positions]
    if seam:
        duplicates = {}
        for first in range(0, len(indices), 3):
            if sum(positions[i][0] for i in indices[first:first + 3]) <= 1.5:
                continue
            for corner in range(first, first + 3):
                original = indices[corner]
                if positions[original][0] != .5:
                    continue
                if original not in duplicates:
                    duplicates[original] = len(positions)
                    positions.append(positions[original])
                    normals.append(normals[original])
                    uv.append((uv[original][0] + 1, uv[original][1]))
                indices[corner] = duplicates[original]
    return positions, normals, uv, indices


def source_bytes(source):
    p, n, uv, indices = source
    data = struct.pack('<II', len(p), len(indices))
    for values, fmt in [(p, '<3f'), (n, '<3f'), (uv, '<2f')]:
        data += b''.join(struct.pack(fmt, *row) for row in values)
    return data + struct.pack(f'<{len(indices)}I', *indices)


def extract_payload(glb):
    length = struct.unpack_from('<I', glb, 12)[0]
    doc = json.loads(glb[20:20 + length])
    primitive = doc['meshes'][0]['primitives'][0]
    view = doc['bufferViews'][primitive['extensions']['DEREN_subtilis']['bufferView']]
    assert view['buffer'] == 0
    begin = 28 + length + view.get('byteOffset', 0)
    return glb[begin:begin + view['byteLength']]


def verify(out):
    manifest = json.loads((out / 'fixtures.json').read_text())
    for fixture in manifest['fixtures']:
        for file in fixture['files']:
            data = (out / file['name']).read_bytes()
            if len(data) != file['bytes'] or sha(data) != file['sha256']:
                raise ValueError(f"fixture content mismatch: {file['name']}")
    print(f"verified {len(manifest['fixtures'])} runtime fixtures")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=Path('source/tests/fixtures/subtilis'))
    parser.add_argument('--converter', type=Path, default=Path('build-subtilis/deren-subtilis.exe'))
    parser.add_argument('--verify', action='store_true')
    args = parser.parse_args()
    if args.verify:
        verify(args.out)
        return
    converter = args.converter.resolve()
    commit = subprocess.check_output(['git', 'log', '-1', '--format=%H', '--',
                                     'tools/subtilis', 'source/asset/subtilis'], text=True).strip()
    manifest = dict(schema='deren-object-lod-fixtures/1', converter_commit=commit,
                    converter_sha256=sha(converter.read_bytes()), fixtures=[])
    args.out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.generate-', dir=args.out.resolve()) as temporary:
        work = Path(temporary)
        for name in ['cross_dag', 'seam', 'lod0_only']:
            for width in ([32, 48, 64] if name == 'cross_dag' else [16]):
                source = geometry(width, name == 'seam')
                if name == 'lod0_only':
                    source = ([(0, 0, 0), (1, 0, 0), (0, 1, 0)], [(0, 0, 1)] * 3,
                              [(0, 0), (1, 0), (0, 1)], [0, 1, 2])
                source_file, cooked, report = work / 'source.glb', work / 'cooked.glb', work / 'build.json'
                source_file.write_bytes(glb_bytes(*source))
                command = [str(converter), 'build', str(source_file), '-o', str(cooked),
                           '--threads', '1', '--report', str(report), '--overwrite']
                subprocess.run(command, check=True, capture_output=True)
                stats = json.loads(report.read_text())['primitives'][0]
                if name != 'cross_dag' or stats['crossGroupRepartitions'] > 0:
                    break
            else:
                raise ValueError('no genuine automatic cross-group DAG witness')
            subprocess.run([str(converter), 'validate', str(cooked), '--full'], check=True)
            outputs = {f'{name}.glb': cooked.read_bytes(),
                       f'{name}.payload': extract_payload(cooked.read_bytes()),
                       f'{name}.source.bin': source_bytes(source)}
            files = []
            for filename, data in outputs.items():
                (args.out / filename).write_bytes(data)
                files.append(dict(name=filename, bytes=len(data), sha256=sha(data)))
            manifest['fixtures'].append(dict(name=name, width=width, files=files,
                cross_group_repartitions=stats['crossGroupRepartitions'],
                command=['deren-subtilis', 'build', '<generated-source.glb>', '-o', f'{name}.glb',
                         '--threads', '1', '--report', '<build.json>', '--overwrite']))
    (args.out / 'fixtures.json').write_text(json.dumps(manifest, indent=2) + '\n')
    verify(args.out)


if __name__ == '__main__':
    main()
