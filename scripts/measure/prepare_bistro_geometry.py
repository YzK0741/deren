"""Prepare the static, geometry-only Bistro acceptance scene (not an asset importer).

Usage: python prepare_bistro_geometry.py <upstream bistro.gltf> <output.gltf>
Requires NumPy. Preserves triangles/UVs/material partitions, bakes node transforms,
and combines instances by material to fit the existing visibility item budget.
Textures, animation and authored lights are deliberately excluded.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path

import numpy as np


def prepare(source, output):
    src = json.loads(source.read_bytes())
    buffers = [(source.parent / item['uri']).read_bytes() for item in src['buffers']]
    assert not src.get('skins'), 'Static scenes only'

    def accessor(index):
        a = src['accessors'][index]
        assert 'sparse' not in a and not a.get('normalized', False)
        v = src['bufferViews'][a['bufferView']]
        dtype = np.dtype({5126: '<f4', 5125: '<u4', 5123: '<u2', 5121: 'u1'}[a['componentType']])
        width = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}[a['type']]
        offset = v.get('byteOffset', 0) + a.get('byteOffset', 0)
        return np.ndarray((a['count'], width), dtype, buffers[v['buffer']], offset,
                          (v.get('byteStride', dtype.itemsize * width), dtype.itemsize)).copy()

    def local(node):
        if 'matrix' in node:
            return np.array(node['matrix'], dtype=np.float64).reshape(4, 4, order='F')
        x, y, z, w = node.get('rotation', [0, 0, 0, 1])
        r = np.array([[1-2*y*y-2*z*z, 2*x*y-2*z*w, 2*x*z+2*y*w],
                      [2*x*y+2*z*w, 1-2*x*x-2*z*z, 2*y*z-2*x*w],
                      [2*x*z-2*y*w, 2*y*z+2*x*w, 1-2*x*x-2*y*y]])
        m = np.eye(4)
        m[:3, :3] = r @ np.diag(node.get('scale', [1, 1, 1]))
        m[:3, 3] = node.get('translation', [0, 0, 0])
        return m

    groups = {}
    source_triangles = 0
    draws = 0

    def visit(index, parent):
        nonlocal source_triangles, draws
        node = src['nodes'][index]
        world = parent @ local(node)
        if 'mesh' in node:
            for primitive in src['meshes'][node['mesh']]['primitives']:
                assert primitive.get('mode', 4) == 4 and not primitive.get('targets')
                attrs = primitive['attributes']
                pos = accessor(attrs['POSITION']).astype(np.float64)
                normal = accessor(attrs['NORMAL']).astype(np.float64)
                uv = accessor(attrs['TEXCOORD_0']) if 'TEXCOORD_0' in attrs else np.zeros((len(pos), 2))
                pos = pos @ world[:3, :3].T + world[:3, 3]
                normal = normal @ np.linalg.inv(world[:3, :3])
                normal /= np.maximum(np.linalg.norm(normal, axis=1, keepdims=True), 1e-30)
                ids = accessor(primitive['indices']).reshape(-1).astype(np.uint32)
                if np.linalg.det(world[:3, :3]) < 0:
                    ids = ids.reshape(-1, 3)[:, [0, 2, 1]].reshape(-1)
                material = primitive.get('material', 0)
                group = groups.setdefault(material, {'p': [], 'n': [], 'uv': [], 'i': [], 'vertices': 0})
                group['i'].append(ids + group['vertices'])
                group['p'].append(pos.astype('<f4'))
                group['n'].append(normal.astype('<f4'))
                group['uv'].append(uv.astype('<f4'))
                group['vertices'] += len(pos)
                source_triangles += len(ids) // 3
                draws += 1
        for child in node.get('children', []):
            visit(child, world)

    for root in src['scenes'][src.get('scene', 0)]['nodes']:
        visit(root, np.eye(4))
    out = {'asset': {'version': '2.0', 'generator': 'deren geometry-only Bistro acceptance'},
           'scene': 0, 'scenes': [{'nodes': [0]}], 'nodes': [{'mesh': 0}],
           'meshes': [{'primitives': []}], 'buffers': [], 'bufferViews': [], 'accessors': [],
           'materials': copy.deepcopy(src['materials'])}
    for material in out['materials']:
        spec = material.get('extensions', {}).get('KHR_materials_pbrSpecularGlossiness', {})
        material['pbrMetallicRoughness'] = {'baseColorFactor': spec.get('diffuseFactor', [1, 1, 1, 1]),
                                          'metallicFactor': 0, 'roughnessFactor': max(.04, 1-spec.get('glossinessFactor', 0))}
        for key in ['normalTexture', 'occlusionTexture', 'emissiveTexture', 'extensions']:
            material.pop(key, None)
        material['emissiveFactor'] = [0, 0, 0]
    payload = bytearray()

    def append(array, kind, component):
        while len(payload) % 4:
            payload.append(0)
        view = len(out['bufferViews'])
        out['bufferViews'].append({'buffer': 0, 'byteOffset': len(payload), 'byteLength': array.nbytes})
        payload.extend(array.tobytes())
        result = len(out['accessors'])
        entry = {'bufferView': view, 'componentType': component, 'count': len(array), 'type': kind}
        if kind == 'VEC3':
            entry.update(min=array.min(axis=0).tolist(), max=array.max(axis=0).tolist())
        out['accessors'].append(entry)
        return result

    written_triangles = 0
    for material, group in sorted(groups.items()):
        attrs = {name: append(np.concatenate(group[key]), kind, 5126)
                 for name, key, kind in [('POSITION', 'p', 'VEC3'), ('NORMAL', 'n', 'VEC3'), ('TEXCOORD_0', 'uv', 'VEC2')]}
        indices = np.concatenate(group['i']).astype('<u4')
        assert int(indices.max()) < group['vertices']
        written_triangles += len(indices) // 3
        out['meshes'][0]['primitives'].append({'attributes': attrs, 'indices': append(indices, 'SCALAR', 5125), 'material': material})
    assert written_triangles == source_triangles
    binary = output.with_suffix('.bin')
    binary.write_bytes(payload)
    out['buffers'] = [{'uri': binary.name, 'byteLength': len(payload)}]
    output.write_text(json.dumps(out))
    stats = {'source': str(source), 'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
             'source_draws': draws, 'merged_draws': len(groups), 'triangles': source_triangles,
             'output_bytes': len(payload), 'geometry_only': True}
    output.with_suffix('.manifest.json').write_text(json.dumps(stats, indent=2))
    print(json.dumps(stats))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    prepare(args.source, args.output)
