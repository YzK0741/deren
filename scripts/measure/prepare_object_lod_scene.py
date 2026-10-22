"""Derive the reviewed TANGENT-free Bistro scene and cook its cluster DAG.

No original files are modified. Large inputs/outputs stay in the requested asset
directory; the manifest binds source, derived JSON, buffers and tool identity.
"""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import shutil
import time


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def prepare(source, out, converter):
    source, out, converter = source.resolve(), out.resolve(), converter.resolve()
    out.mkdir(parents=True, exist_ok=True)
    original = json.loads(source.read_text(encoding='utf-8'))
    derived = copy.deepcopy(original)
    removed = 0
    for mesh in derived['meshes']:
        for primitive in mesh['primitives']:
            removed += primitive.get('attributes', {}).pop('TANGENT', None) is not None
    if not removed:
        raise ValueError('expected a TANGENT-bearing source')
    buffers = []
    for buffer in derived.get('buffers', []):
        uri = buffer.get('uri', '')
        if not uri or uri.startswith('data:'):
            raise ValueError('this scene preparation expects external binary buffers')
        path = (source.parent / uri).resolve()
        buffers.append({'path': path.as_posix(), 'bytes': path.stat().st_size, 'sha256': sha(path)})
        buffer['uri'] = Path(os.path.relpath(path, out)).as_posix()
    # Verify the derivation changes exactly the agreed attribute and buffer locations.
    restored = copy.deepcopy(derived)
    for old, new in zip(original['meshes'], restored['meshes'], strict=True):
        for op, np in zip(old['primitives'], new['primitives'], strict=True):
            if 'TANGENT' in op.get('attributes', {}):
                np['attributes']['TANGENT'] = op['attributes']['TANGENT']
    for old, new in zip(original['buffers'], restored['buffers'], strict=True):
        new['uri'] = old['uri']
    if restored != original:
        raise ValueError('unexpected scene derivation change')
    destination = out / 'bistro-object-lod.gltf'
    raw = (json.dumps(derived, separators=(',', ':')) + '\n').encode()
    if destination.exists() and destination.read_bytes() != raw:
        raise ValueError(f'refusing to overwrite different derived scene: {destination}')
    destination.write_bytes(raw)
    return {'source': {'path': source.as_posix(), 'sha256': sha(source)},
            'derived': {'path': destination.as_posix(), 'sha256': sha(destination)},
            'removed_tangents': removed, 'buffers': buffers,
            'converter': {'path': converter.as_posix(), 'sha256': sha(converter)},
            'rule': 'remove TANGENT attributes; rewrite external buffer URIs only'}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', type=Path, default=Path('D:/deren-assets/bistro/bistro-geometry.gltf'))
    p.add_argument('--out', type=Path, default=Path('D:/deren-assets/2026-10-21-object-lod'))
    p.add_argument('--converter', type=Path, default=Path('build-subtilis/deren-subtilis.exe'))
    p.add_argument('--prepare-only', action='store_true')
    p.add_argument('--cook-dir', type=Path, help='optional local staging directory; verified outputs are copied to --out')
    p.add_argument('--threads', type=int, default=12)
    a = p.parse_args()
    if a.threads <= 0:
        p.error('--threads must be positive')
    manifest = prepare(a.source, a.out, a.converter)
    manifest_path = a.out / 'asset-manifest.json'
    if a.prepare_only and manifest_path.exists():
        previous = json.loads(manifest_path.read_text(encoding='utf-8'))
        for key in ('source', 'derived', 'buffers', 'converter', 'removed_tangents', 'rule'):
            if previous.get(key) != manifest[key]:
                raise ValueError(f'existing manifest identity differs: {key}')
        if 'build' in previous:
            manifest = previous
    if not a.prepare_only:
        cook_dir = (a.cook_dir or a.out).resolve()
        cook_dir.mkdir(parents=True, exist_ok=True)
        cooked = cook_dir / 'bistro-object-lod.glb'
        report = cook_dir / 'converter-build.json'
        if cooked.exists():
            raise ValueError(f'cooked output already exists; inspect its manifest instead of overwriting: {cooked}')
        command = [str(a.converter.resolve()), 'build', manifest['derived']['path'], '-o', str(cooked),
                   '--threads', str(a.threads), '--report', str(report)]
        started = time.perf_counter()
        with (cook_dir / 'converter-build.log').open('w', encoding='utf-8') as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
        manifest['build'] = {'command': command, 'wall_seconds': time.perf_counter() - started,
                             'path': cooked.as_posix(), 'bytes': cooked.stat().st_size,
                             'sha256': sha(cooked), 'report_sha256': sha(report)}
        with (cook_dir / 'converter-full-validate.log').open('w', encoding='utf-8') as log:
            subprocess.run([str(a.converter.resolve()), 'validate', str(cooked), '--full'],
                           stdout=log, stderr=subprocess.STDOUT, check=True)
        manifest['full_validation'] = 'passed'
        if cook_dir != a.out.resolve():
            for name in ('bistro-object-lod.glb', 'converter-build.json', 'converter-build.log', 'converter-full-validate.log'):
                target = a.out.resolve() / name
                if target.exists() and name == 'bistro-object-lod.glb':
                    raise ValueError(f'refusing to overwrite archive: {target}')
                shutil.copyfile(cook_dir / name, target)
                if sha(target) != sha(cook_dir / name):
                    raise ValueError(f'archive copy mismatch: {name}')
            manifest['build']['staging_path'] = cooked.as_posix()
            manifest['build']['path'] = (a.out.resolve() / cooked.name).as_posix()
    manifest_path.write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'removed_tangents': manifest['removed_tangents'], 'cooked': 'build' in manifest,
                      'manifest': str(a.out / 'asset-manifest.json')}))


if __name__ == '__main__':
    main()
