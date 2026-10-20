"""Verify the textured Bistro acceptance asset against the pinned upstream glTF.

Checks (all read-only, standard library only):
  * node / mesh / material counts are the upstream hierarchy, unmerged;
  * the exported buffers are LOCAL copies (no `../` references) and byte-identical to the upstream bins;
  * every mesh's index count is unchanged and the total triangle count matches the upstream file;
  * every material references only core metallic/roughness slots (no spec/gloss or specular/ior extension left);
  * every referenced image file exists and the referenced-texture / runtime-slot counts are the measured ones;
  * the intermediate spec/gloss glTF still carries the 343 decoded textures beside the same geometry.

Usage: python verify_bistro_textured.py <prepared-dir> <upstream-dir>
Exit code 0 = every check passed; 1 = at least one failure (printed).
"""
import argparse
import hashlib
import json
import sys
from pathlib import Path

EXPECTED_CORE = {
    'nodes': 6006,
    'meshes': 551,
    'materials': 254,
    'textures': 811,          # 343 decoded source textures + 2 per converted material, unused records included
    'referenced_textures': 597,
    'converted_materials': 234,
    'triangle_records': 1753630,
}
EXPECTED_SPECGLOSS = {'nodes': 6006, 'meshes': 551, 'materials': 254, 'textures': 343}


def file_sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b''):
            digest.update(chunk)
    return digest.hexdigest()


def triangles(scene):
    total = 0
    accessors = scene['accessors']
    per_mesh = []
    for mesh in scene['meshes']:
        count = 0
        for primitive in mesh['primitives']:
            if 'indices' in primitive:
                count += accessors[primitive['indices']]['count'] // 3
        per_mesh.append(count)
        total += count
    return total, per_mesh


def referenced_textures(scene):
    indices = set()
    for material in scene['materials']:
        pbr = material.get('pbrMetallicRoughness', {})
        for key in ('baseColorTexture', 'metallicRoughnessTexture'):
            if key in pbr:
                indices.add(pbr[key]['index'])
        for key in ('normalTexture', 'occlusionTexture', 'emissiveTexture'):
            if key in material:
                indices.add(material[key]['index'])
    return indices


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('prepared', type=Path, help='directory holding bistro-core-mr.gltf and the isolated buffers')
    parser.add_argument('upstream', type=Path, help='directory holding the pinned bistro.gltf and its bins')
    args = parser.parse_args()

    failures = []

    def check(condition, message):
        if not condition:
            failures.append(message)
        print(('PASS ' if condition else 'FAIL ') + message)

    prepared = args.prepared.resolve()
    upstream = args.upstream.resolve()
    core = json.loads((prepared / 'bistro-core-mr.gltf').read_bytes())
    specgloss = json.loads((prepared / 'bistro-specgloss.gltf').read_bytes())
    source = json.loads((upstream / 'bistro.gltf').read_bytes())

    for name, expected in EXPECTED_SPECGLOSS.items():
        check(len(specgloss[name]) == expected, f'spec/gloss {name} == {expected} (got {len(specgloss[name])})')
    for name, expected in EXPECTED_CORE.items():
        if name in ('referenced_textures', 'converted_materials', 'triangle_records'):
            continue
        check(len(core[name]) == expected, f'core-MR {name} == {expected} (got {len(core[name])})')

    # A CONVERTED material is one whose base colour now points at a texture the converter APPENDED (record
    # 343 onward: the 343 decoded source records come first, then two new records per converted material).
    converted = sum(1 for material in core['materials']
                    if material.get('pbrMetallicRoughness', {}).get('baseColorTexture', {}).get('index', 0) >= 343)
    check(converted == EXPECTED_CORE['converted_materials'], f'converted materials == {EXPECTED_CORE["converted_materials"]} (got {converted})')
    check(all('KHR_materials_pbrSpecularGlossiness' not in material.get('extensions', {}) for material in core['materials']),
          'no spec/gloss material extension remains')
    check('KHR_materials_pbrSpecularGlossiness' not in core.get('extensionsUsed', []), 'the spec/gloss extension is not in extensionsUsed')
    check(sorted(core.get('extensionsUsed', [])) == ['KHR_lights_punctual', 'KHR_materials_transmission'],
          f'core-MR extensionsUsed is exactly transmission + lights (got {core.get("extensionsUsed")})')

    upstream_total, upstream_per_mesh = triangles(source)
    core_total, core_per_mesh = triangles(core)
    check(upstream_total == EXPECTED_CORE['triangle_records'], f'upstream triangle records == {EXPECTED_CORE["triangle_records"]} (got {upstream_total})')
    check(core_total == upstream_total, f'geometry triangle total unchanged (upstream {upstream_total}, prepared {core_total})')
    check(core_per_mesh == upstream_per_mesh, 'every mesh keeps its upstream triangle count')

    upstream_buffers = [upstream / buffer['uri'] for buffer in source['buffers']]
    for number, buffer in enumerate(core['buffers']):
        uri = buffer['uri']
        check('..' not in uri and '/' not in uri, f'buffer {number} is a local file name ({uri})')
        copied = prepared / uri
        check(copied.is_file(), f'buffer {number} exists ({uri})')
        if copied.is_file() and number < len(upstream_buffers) and upstream_buffers[number].is_file():
            same = file_sha256(copied) == file_sha256(upstream_buffers[number])
            check(same, f'buffer {number} is byte-identical to the upstream bin')
    check(len(core['buffers']) == len(source['buffers']), 'the binary buffer count is unchanged')

    referenced = referenced_textures(core)
    check(len(referenced) == EXPECTED_CORE['referenced_textures'], f'referenced texture records == {EXPECTED_CORE["referenced_textures"]} (got {len(referenced)})')
    referenced_images = {core['textures'][index]['source'] for index in referenced}
    check(len(referenced_images) == EXPECTED_CORE['referenced_textures'], f'referenced image files == {EXPECTED_CORE["referenced_textures"]} (got {len(referenced_images)})')
    missing = [image['uri'] for image in core['images'] if not (prepared / image['uri']).is_file()]
    check(not missing, f'all {len(core["images"])} image files exist (missing: {len(missing)})')
    check(all('..' not in image['uri'] and '/' not in image['uri'] for image in core['images']), 'every image is referenced beside the glTF')

    for name in ('source-buffer-0.bin', 'source-buffer-1.bin'):
        check((prepared / name).is_file(), f'isolated buffer present: {name}')

    print()
    if failures:
        print(f'VERIFY FAILED: {len(failures)} check(s) failed')
        return 1
    print('VERIFY PASSED: the textured acceptance asset is the pinned upstream hierarchy with isolated, unchanged buffers')
    return 0


if __name__ == '__main__':
    sys.exit(main())
