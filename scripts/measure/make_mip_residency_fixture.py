"""Generate a textured NPOT quad for missing-mip GPU capture tests."""
import argparse
import base64
import io
import json
from pathlib import Path
import struct
from PIL import Image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    image = Image.new('RGBA', (513, 257))
    image.putdata([((x*13) % 256, (y*19) % 256, 80 if (x//16+y//16) % 2 else 220, 255)
                   for y in range(257) for x in range(513)])
    png = io.BytesIO()
    image.save(png, format='PNG')
    positions = [-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0]
    raw = struct.pack('<12f12f8f6H', *positions, *([0, 0, 1]*4),
                      *[0, 1, 1, 1, 1, 0, 0, 0], *[0, 1, 2, 0, 2, 3])
    scene = {
        'asset': {'version': '2.0'}, 'scene': 0, 'scenes': [{'nodes': [0]}],
        'nodes': [{'mesh': 0}],
        'meshes': [{'primitives': [{'attributes': {'POSITION': 0, 'NORMAL': 1, 'TEXCOORD_0': 2},
                                   'indices': 3, 'material': 0}]}],
        'materials': [{'doubleSided': True, 'pbrMetallicRoughness': {
            'baseColorTexture': {'index': 0}, 'metallicFactor': 0, 'roughnessFactor': 1}}],
        'textures': [{'source': 0}],
        'images': [{'uri': 'data:image/png;base64,'+base64.b64encode(png.getvalue()).decode()}],
        'buffers': [{'byteLength': len(raw), 'uri': 'data:application/octet-stream;base64,'+base64.b64encode(raw).decode()}],
        'bufferViews': [{'buffer': 0, 'byteOffset': offset, 'byteLength': size}
                        for offset, size in [(0, 48), (48, 48), (96, 32), (128, 12)]],
        'accessors': [
            {'bufferView': 0, 'componentType': 5126, 'count': 4, 'type': 'VEC3', 'min': [-1, -1, 0], 'max': [1, 1, 0]},
            {'bufferView': 1, 'componentType': 5126, 'count': 4, 'type': 'VEC3'},
            {'bufferView': 2, 'componentType': 5126, 'count': 4, 'type': 'VEC2'},
            {'bufferView': 3, 'componentType': 5123, 'count': 6, 'type': 'SCALAR'}]}
    (args.output/'scene.gltf').write_text(json.dumps(scene))


if __name__ == '__main__':
    main()
