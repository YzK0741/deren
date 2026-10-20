"""Decode Bistro DDS textures to core glTF PNG references without merging nodes.
Requires Pillow. Follow with convert_bistro_metalrough.py for the spec/gloss materials, then
verify_bistro_textured.py. glTF Transform is deliberately NOT used: its output relies on
KHR_materials_specular/ior, which this engine does not support, and its ../bistro.bin buffer URI
rewrote the pinned upstream bin once (restored and hash-checked since).
Usage: python prepare_bistro_textures.py <bistro.gltf> <output-dir> [--size 1024]
"""
import argparse
import copy
import json
import shutil
from pathlib import Path
from PIL import Image


def prepare(source, output, size):
    assert size > 0
    source = source.resolve()
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    scene = json.loads(source.read_bytes())
    images = []
    textures = []
    converted = {}
    for texture in scene['textures']:
        dds = texture.get('extensions', {}).get('MSFT_texture_dds', {}).get('source')
        index = dds if dds is not None else texture['source']
        uri = scene['images'][index]['uri']
        if uri not in converted:
            path = (source.parent / uri).resolve()
            assert path.is_relative_to(source.parent), 'External texture path'
            with Image.open(path) as original:
                original.load()
                image = original.convert('RGBA')
                image.thumbnail((size, size), Image.Resampling.LANCZOS)
                name = f'texture-{len(images):04d}.png'
                image.save(output / name)
                print(f'{len(images)+1}: {path.name} -> {name} {image.size}', flush=True)
            converted[uri] = len(images)
            images.append({'uri': name})
        value = copy.deepcopy(texture)
        value['source'] = converted[uri]
        value.get('extensions', {}).pop('MSFT_texture_dds', None)
        if not value.get('extensions'):
            value.pop('extensions', None)
        textures.append(value)
    scene['images'] = images
    scene['textures'] = textures
    for key in ['extensionsUsed', 'extensionsRequired']:
        if key in scene:
            scene[key] = [x for x in scene[key] if x != 'MSFT_texture_dds']
    for number, buffer in enumerate(scene['buffers']):
        name = f'source-buffer-{number}.bin'
        shutil.copyfile(source.parent / buffer['uri'], output / name)
        buffer['uri'] = name
    (output / 'bistro-specgloss.gltf').write_text(json.dumps(scene))
    (output / 'preparation.json').write_text(json.dumps({'textures': len(images), 'max_size': size,
        'nodes_preserved': len(scene['nodes']), 'materials_preserved': len(scene['materials']),
        'notes': 'DDS decoded to RGBA PNG, alpha retained, no mesh or node merging; material conversion pending.'}, indent=2))
    print('Prepared', len(images), 'textures; node hierarchy preserved', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--size', type=int, default=1024)
    args = parser.parse_args()
    prepare(args.source, args.output, args.size)
