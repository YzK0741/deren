"""Write a standalone planar grid glTF for CPU/GPU raster comparison.

Cook with deren-subtilis build, then capture at camera 0,0,4,0,0,0 with
software_raster_threshold_px=4096 to exercise the whole software producer.
The large threshold is a correctness control, not a recommended runtime setting.
"""
import argparse
import base64
import json
import math
from pathlib import Path
import struct


def write_fixture(output: Path, subdivisions: int = 24, slope: float = 0.0):
    n = subdivisions
    positions = [(x / n * 2 - 1, y / n * 2 - 1, 0.0 if slope == 0 else slope * ((x + y) / n * 2 - 2)) for y in range(n + 1) for x in range(n + 1)]
    normal = (0., 0., 1.) if slope == 0 else tuple(v / math.sqrt(1 + 2 * slope * slope) for v in (-slope, -slope, 1.))
    indices = []
    for y in range(n):
        for x in range(n):
            a = y * (n + 1) + x
            indices.extend([a, a + 1, a + n + 2, a, a + n + 2, a + n + 1])
    chunks = [b''.join(struct.pack('<3f', *p) for p in positions),
              struct.pack('<' + 'f' * len(positions) * 3, *(normal * len(positions))),
              struct.pack('<' + 'I' * len(indices), *indices)]
    data = b''.join(chunks)
    views = []
    offset = 0
    for chunk in chunks:
        views.append(dict(buffer=0, byteOffset=offset, byteLength=len(chunk)))
        offset += len(chunk)
    scene = {'asset': {'version': '2.0'},
             'buffers': [{'uri': 'data:application/octet-stream;base64,' + base64.b64encode(data).decode(), 'byteLength': len(data)}],
             'bufferViews': views,
             'accessors': [dict(bufferView=0, componentType=5126, count=len(positions), type='VEC3', min=[-1,-1,-2*slope], max=[1,1,2*slope]),
                           dict(bufferView=1, componentType=5126, count=len(positions), type='VEC3'),
                           dict(bufferView=2, componentType=5125, count=len(indices), type='SCALAR')],
             'materials': [{'doubleSided': True}],
             'meshes': [{'primitives': [{'attributes': {'POSITION': 0, 'NORMAL': 1}, 'indices': 2, 'material': 0}]}],
             'nodes': [{'mesh': 0}], 'scenes': [{'nodes': [0]}], 'scene': 0}
    output.write_text(json.dumps(scene), encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--subdivisions', type=int, default=24)
    parser.add_argument('--slope', type=float, default=0.0)
    args = parser.parse_args()
    if not 8 <= args.subdivisions <= 128:
        parser.error('subdivisions must be between 8 and 128')
    if not math.isfinite(args.slope) or not 0 <= args.slope <= 0.5:
        parser.error('slope must be finite and between 0 and 0.5')
    if args.output.exists():
        parser.error('output already exists; choose a fresh path')
    write_fixture(args.output, args.subdivisions, args.slope)
