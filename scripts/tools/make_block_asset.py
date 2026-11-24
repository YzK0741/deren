"""Writes the small unit block asset used by the demo scenes.

The helmet demo marks its floors, walls and the sliding door with collision boxes. A player cannot see a
collision box, so the demo also draws the same volumes as this block: one 1x1x1 cube centered on the
origin, so a node whose scale matches its collision box draws exactly the volume it collides with.

Usage: python scripts/tools/make_block_asset.py [asset/block]
"""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path

# Four corners per face, counter-clockwise seen from outside, with the face normal.
FACES = (
    ((0.0, 0.0, 1.0), ((-0.5, -0.5, 0.5), (0.5, -0.5, 0.5), (0.5, 0.5, 0.5), (-0.5, 0.5, 0.5))),
    ((0.0, 0.0, -1.0), ((0.5, -0.5, -0.5), (-0.5, -0.5, -0.5), (-0.5, 0.5, -0.5), (0.5, 0.5, -0.5))),
    ((1.0, 0.0, 0.0), ((0.5, -0.5, 0.5), (0.5, -0.5, -0.5), (0.5, 0.5, -0.5), (0.5, 0.5, 0.5))),
    ((-1.0, 0.0, 0.0), ((-0.5, -0.5, -0.5), (-0.5, -0.5, 0.5), (-0.5, 0.5, 0.5), (-0.5, 0.5, -0.5))),
    ((0.0, 1.0, 0.0), ((-0.5, 0.5, 0.5), (0.5, 0.5, 0.5), (0.5, 0.5, -0.5), (-0.5, 0.5, -0.5))),
    ((0.0, -1.0, 0.0), ((-0.5, -0.5, -0.5), (0.5, -0.5, -0.5), (0.5, -0.5, 0.5), (-0.5, -0.5, 0.5))),
)


def build_geometry() -> tuple[bytes, bytes, bytes]:
    positions: list[float] = []
    normals: list[float] = []
    indices: list[int] = []
    for normal, corners in FACES:
        base = len(positions) // 3
        for corner in corners:
            positions.extend(corner)
            normals.extend(normal)
        indices.extend((base, base + 1, base + 2, base, base + 2, base + 3))
    return (
        struct.pack(f"<{len(positions)}f", *positions),
        struct.pack(f"<{len(normals)}f", *normals),
        struct.pack(f"<{len(indices)}H", *indices),
    )


def write(directory: Path) -> None:
    positions, normals, indices = build_geometry()
    # One buffer: positions, then normals, then indices; keep the index block four-byte aligned.
    normal_offset = len(positions)
    index_offset = normal_offset + len(normals)
    index_offset += (-index_offset) % 4
    padding = index_offset - normal_offset - len(normals)
    buffer = positions + normals + b"\0" * padding + indices
    (directory / "block.bin").write_bytes(buffer)
    document = {
        "asset": {"version": "2.0", "generator": "make_block_asset.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": "Block"}],
        "meshes": [{
            "name": "Block",
            "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2, "material": 0}],
        }],
        "materials": [{
            "name": "Block",
            "pbrMetallicRoughness": {"baseColorFactor": [0.62, 0.63, 0.66, 1.0], "metallicFactor": 0.0, "roughnessFactor": 0.85},
            "doubleSided": False,
        }],
        "buffers": [{"uri": "block.bin", "byteLength": len(buffer)}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": len(positions), "target": 34962},
            {"buffer": 0, "byteOffset": normal_offset, "byteLength": len(positions), "target": 34962},
            {"buffer": 0, "byteOffset": index_offset, "byteLength": len(indices), "target": 34963},
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": len(positions) // 12, "type": "VEC3",
             "min": [-0.5, -0.5, -0.5], "max": [0.5, 0.5, 0.5]},
            {"bufferView": 1, "componentType": 5126, "count": len(positions) // 12, "type": "VEC3"},
            {"bufferView": 2, "componentType": 5123, "count": len(indices) // 2, "type": "SCALAR"},
        ],
    }
    (directory / "block.gltf").write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path, nargs="?", default=Path("asset/block"))
    directory = parser.parse_args().directory.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    write(directory)
    print(f"Block asset written: {directory}")


if __name__ == "__main__":
    main()
