"""Write a standalone planar-grid glTF for the CPU/GPU software-raster comparison.

Deterministic fixture generation for the M4 software-raster acceptance (the closeout checkpoint asked for this
generator to be standalone, with no dependency on leftovers under tmp/). Two shapes are supported:

  * the planar grid (default) - the raster correctness fixture;
  * `--overlap` - the SAME grid instanced three times: two coplanar duplicates (equal-depth ID tie) plus one
    farther copy 0.15 behind them, the "coplanar duplicate items plus farther overlapping item" fixture.

The script writes only the JSON .glTF; cooking it with the offline converter is the documented next step:

    build-subtilis/deren-subtilis.exe build <fixture>.gltf -o <fixture>.glb --report <report>.json
    python scripts/measure/capture_software_gpu_route.py --asset <fixture>.glb ...

The large threshold used by the capture harness (4096 px) is a CORRECTNESS control that forces the software path;
it is not a recommended runtime setting. `--self-test` writes the fixtures twice into a scratch directory, compares
the bytes (determinism) and exercises the refusal paths - no GPU and no converter are involved.
"""
import argparse
import base64
import hashlib
import json
import math
from pathlib import Path
import struct
import tempfile


def fixture_bytes(subdivisions: int = 24, slope: float = 0.0, overlap: bool = False, occluder: bool = False) -> bytes:
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
    # THE OVERLAP SHAPE: two coplanar instances of the SAME mesh (equal depth, so the software ID ordering tie is
    # exercised) plus one instance 0.15 farther from the pinned camera (0,0,4) - which the hardware depth still sees
    # but the nearer coplanar pair wins, exactly the fixture the acceptance captures use.
    if occluder:
        # THE OCCLUSION SHAPE: one plane scaled down in front of a second, larger plane 1.5 units behind it. The
        # history HZB can cull the hidden back clusters, and a camera sweep makes some of them visible again - which
        # is what a nonzero recovery capture needs. Both instances share the one cooked mesh (all geometry, no
        # LOD0-fallback items), so the CPU reference oracle can rasterize every candidate.
        nodes = [{'mesh': 0, 'scale': [0.35, 0.35, 0.35]}, {'mesh': 0, 'translation': [0, 0, -1.5], 'scale': [1.5, 1.5, 1.5]}]
    elif overlap:
        nodes = [{'mesh': 0}, {'mesh': 0}, {'mesh': 0, 'translation': [0, 0, -0.15]}]
    else:
        nodes = [{'mesh': 0}]
    scene = {'asset': {'version': '2.0'},
             'buffers': [{'uri': 'data:application/octet-stream;base64,' + base64.b64encode(data).decode(), 'byteLength': len(data)}],
             'bufferViews': views,
             'accessors': [dict(bufferView=0, componentType=5126, count=len(positions), type='VEC3', min=[-1, -1, -2 * slope], max=[1, 1, 2 * slope]),
                           dict(bufferView=1, componentType=5126, count=len(positions), type='VEC3'),
                           dict(bufferView=2, componentType=5125, count=len(indices), type='SCALAR')],
             'materials': [{'doubleSided': True}],
             'meshes': [{'primitives': [{'attributes': {'POSITION': 0, 'NORMAL': 1}, 'indices': 2, 'material': 0}]}],
             'nodes': nodes, 'scenes': [{'nodes': list(range(len(nodes)))}], 'scene': 0}
    return (json.dumps(scene) + '\n').encode('utf-8')


def write_fixture(output: Path, subdivisions: int = 24, slope: float = 0.0, overlap: bool = False, occluder: bool = False) -> None:
    # `xb` = create-new-only: a fresh fixture path is part of the reproducibility contract, and an accidental
    # overwrite would silently change what an old capture was compared against.
    with output.open('xb') as stream:
        stream.write(fixture_bytes(subdivisions, slope, overlap, occluder))


def self_test() -> None:
    scratch = Path(__file__).resolve().parents[2] / 'tmp'
    scratch.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(dir=scratch) as folder:
        root = Path(folder)
        first, second = root / 'a.gltf', root / 'b.gltf'
        write_fixture(first)
        write_fixture(second)
        assert first.read_bytes() == second.read_bytes(), 'planar fixture generation is not deterministic'
        overlap = root / 'tie.gltf'
        write_fixture(overlap, overlap=True)
        scene = json.loads(overlap.read_text(encoding='utf-8'))
        assert len(scene['nodes']) == 3 and scene['nodes'][2]['translation'] == [0, 0, -0.15]
        assert scene['nodes'][0] == scene['nodes'][1] == {'mesh': 0}
        sloped = root / 'slope.gltf'
        write_fixture(sloped, slope=0.25)
        sloped_scene = json.loads(sloped.read_text(encoding='utf-8'))
        assert sloped_scene['accessors'][0]['min'] == [-1, -1, -0.5]
        assert fixture_bytes() != fixture_bytes(overlap=True)
        occluded = root / 'occluder.gltf'
        write_fixture(occluded, occluder=True)
        occluded_scene = json.loads(occluded.read_text(encoding='utf-8'))
        assert len(occluded_scene['nodes']) == 2
        assert occluded_scene['nodes'][0]['scale'] == [0.35, 0.35, 0.35]
        assert occluded_scene['nodes'][1]['translation'] == [0, 0, -1.5]
        assert not fixture_bytes(occluder=True) == fixture_bytes()
        # The output path is a fresh file: an existing path is refused rather than overwritten.
        try:
            write_fixture(first)
        except FileExistsError:
            pass
        else:
            raise AssertionError('overwrote an existing fixture path')
    print('PASS: software-raster fixture generator determinism and overlap/anchor controls')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path, nargs='?')
    parser.add_argument('--subdivisions', type=int, default=24)
    parser.add_argument('--slope', type=float, default=0.0)
    parser.add_argument('--overlap', action='store_true', help='two coplanar instances plus one farther instance')
    parser.add_argument('--occluder', action='store_true', help='a small near plane in front of a larger far plane (recovery fixture)')
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if args.output is None:
        parser.error('output is required unless --self-test is used')
    if not 8 <= args.subdivisions <= 128:
        parser.error('subdivisions must be between 8 and 128')
    if not math.isfinite(args.slope) or not 0 <= args.slope <= 0.5:
        parser.error('slope must be finite and between 0 and 0.5')
    if args.output.exists():
        parser.error('output already exists; choose a fresh path')
    if args.overlap and args.occluder:
        parser.error('--overlap and --occluder are different fixtures; choose one')
    write_fixture(args.output, args.subdivisions, args.slope, args.overlap, args.occluder)
    digest = hashlib.sha256(args.output.read_bytes()).hexdigest()
    print(f'wrote {args.output} ({args.output.stat().st_size} bytes, sha256 {digest})')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
