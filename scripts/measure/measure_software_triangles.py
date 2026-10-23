"""Interleaved three-round A/B of tiled and triangle software raster against hardware-only HZB.

Uses the same textured asset, camera poses, threshold, frame count and executable for all modes.
No shader/build may change during capture. Output directory must be fresh.
"""
import argparse
import json
from pathlib import Path
import statistics

from measure_render_optimizations import run_one, sha256, shader_bundle_sha256


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work', required=True, type=Path)
    parser.add_argument('--asset', required=True, type=Path)
    parser.add_argument('--exe', type=Path, default=Path('build-release-dyn-clang64/deren.exe'))
    args = parser.parse_args()
    work, asset, exe = args.work.resolve(), args.asset.resolve(), args.exe.resolve()
    work.mkdir(parents=True, exist_ok=False)
    for mode in ('hardware', 'tiled', 'triangles'):
        (work / mode).mkdir()
    result = {'complete': False, 'executable_sha256': sha256(exe), 'asset_sha256': sha256(asset),
              'shader_bundle_sha256': shader_bundle_sha256(exe.parent / 'shaders'), 'runs': []}
    output = work / 'results.json'
    orders = [('tiled', 'triangles', 'hardware'), ('hardware', 'triangles', 'tiled'), ('triangles', 'tiled', 'hardware')]
    for round_id, modes in enumerate(orders, 1):
        for camera in ('far', 'near'):
            for mode in modes:
                row = run_one(exe, asset, work / mode, 'hzb' if mode == 'hardware' else 'all_enabled',
                              camera, round_id, 130, triangles=mode == 'triangles')
                if row.get('gpu_frames') != 129 or not row.get('hzb_cull_active'):
                    raise ValueError(f'{row["tag"]}: incomplete timing or inactive HZB')
                if bool(row.get('software_raster_active')) != (mode != 'hardware'):
                    raise ValueError(f'{row["tag"]}: software feature mismatch')
                row['mode'] = mode
                result['runs'].append(row)
                output.write_text(json.dumps(result, indent=2) + '\n')
    result['summary'] = {}
    for mode in ('hardware', 'tiled', 'triangles'):
        result['summary'][mode] = {}
        for camera in ('far', 'near'):
            rows = [r for r in result['runs'] if r['camera'] == camera and r['mode'] == mode]
            result['summary'][mode][camera] = {key: statistics.median(r[key] for r in rows)
                for key in ('gpu_ms', 'cpu_active_ms', 'cut_wall_us', 'software_raster_ms', 'visibility_raster_ms')}
    if sha256(exe) != result['executable_sha256'] or shader_bundle_sha256(exe.parent / 'shaders') != result['shader_bundle_sha256']:
        raise ValueError('executable or shader bundle changed during measurement')
    result['complete'] = True
    output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result['summary'], indent=2), flush=True)


if __name__ == '__main__':
    main()
