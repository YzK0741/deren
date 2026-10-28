"""Generate the deterministic subtilis converter measurement fixtures.

Two self-contained GLB files are written: a small grid (single coarse primitive, fast smoke
asset) and a synthetic large grid (many clusters/groups, enough work to expose the parallel
scheduler). Both are flat 2x2-unit grids with POSITION/NORMAL/TEXCOORD_0 and triangle indices,
so they match the converter profile exactly; nothing is downloaded.

Usage: python make_subtilis_converter_fixtures.py --out-dir <dir> [--small 32] [--large 256]
"""

import argparse
import hashlib
import json
import struct
from pathlib import Path


def grid_geometry(width):
    positions, normals, uv0, indices = [], [], [], []
    for y in range(width + 1):
        for x in range(width + 1):
            positions.append((2.0 * x / width, 2.0 * y / width, 0.0))
            normals.append((0.0, 0.0, 1.0))
            uv0.append((x / width, y / width))
    for y in range(width):
        for x in range(width):
            a = y * (width + 1) + x
            b, c, d = a + 1, a + width + 1, a + width + 2
            indices += [a, b, d, a, d, c]
    return positions, normals, uv0, indices


def glb_bytes(positions, normals, uv0, indices):
    payload = bytearray()
    for row in positions:
        payload += struct.pack("<3f", *row)
    for row in normals:
        payload += struct.pack("<3f", *row)
    for row in uv0:
        payload += struct.pack("<2f", *row)
    index_offset = len(payload)
    for value in indices:
        payload += struct.pack("<I", value)
    while len(payload) % 4:
        payload += b"\x00"
    document = {
        "asset": {"version": "2.0", "generator": "deren-subtilis-converter measurement fixture"},
        "buffers": [{"byteLength": len(payload)}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": len(positions) * 12},
            {"buffer": 0, "byteOffset": len(positions) * 12, "byteLength": len(normals) * 12},
            {"buffer": 0, "byteOffset": len(positions) * 12 + len(normals) * 12, "byteLength": len(uv0) * 8},
            {"buffer": 0, "byteOffset": index_offset, "byteLength": len(indices) * 4},
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": len(positions), "type": "VEC3",
             "min": [min(p[0] for p in positions), min(p[1] for p in positions), min(p[2] for p in positions)],
             "max": [max(p[0] for p in positions), max(p[1] for p in positions), max(p[2] for p in positions)]},
            {"bufferView": 1, "componentType": 5126, "count": len(normals), "type": "VEC3"},
            {"bufferView": 2, "componentType": 5126, "count": len(uv0), "type": "VEC2"},
            {"bufferView": 3, "componentType": 5125, "count": len(indices), "type": "SCALAR"},
        ],
        "meshes": [{"primitives": [{
            "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
            "indices": 3, "material": 0}]}],
        "materials": [{}],
        "nodes": [{"mesh": 0}, {"mesh": 0, "translation": [3, 0, 0]}],
        "scenes": [{"nodes": [0, 1]}],
        "scene": 0,
    }
    encoded = json.dumps(document, separators=(",", ":")).encode()
    while len(encoded) % 4:
        encoded += b" "
    total = 12 + 8 + len(encoded) + 8 + len(payload)
    out = struct.pack("<III", 0x46546C67, 2, total)
    out += struct.pack("<II", len(encoded), 0x4E4F534A) + encoded
    out += struct.pack("<II", len(payload), 0x004E4942) + bytes(payload)
    return out


def write_asset(out_dir, name, width):
    positions, normals, uv0, indices = grid_geometry(width)
    data = glb_bytes(positions, normals, uv0, indices)
    path = out_dir / f"{name}.glb"
    path.write_bytes(data)
    return {
        "name": name,
        "path": str(path),
        "sha256": hashlib.sha256(data).hexdigest(),
        "bytes": len(data),
        "triangles": len(indices) // 3,
        "vertices": len(positions),
        "grid_width": width,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--small", type=int, default=32)
    parser.add_argument("--large", type=int, default=256)
    args = parser.parse_args()
    args.out_dir.mkdir(parents=True, exist_ok=True)
    manifest = {
        "schema": "subtilis-converter-fixtures/1",
        "fixtures": [
            write_asset(args.out_dir, "small", args.small),
            write_asset(args.out_dir, "large", args.large),
        ],
    }
    manifest_path = args.out_dir / "fixtures.json"
    manifest_path.write_text(json.dumps(manifest, indent=2))
    for fixture in manifest["fixtures"]:
        print(f"{fixture['name']}: {fixture['bytes']} bytes, {fixture['triangles']} triangles, sha256 {fixture['sha256']}")
    print(f"manifest: {manifest_path}")


if __name__ == "__main__":
    main()
