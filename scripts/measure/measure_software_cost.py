"""Measure an individual software-raster optimization stage with fixed settings.

Uses the corrected all-enabled baseline's asset/cameras/threshold and three rounds.
Keep each stage in a fresh output directory and retain its executable/shader hashes.
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
    parser.add_argument('--triangles', action='store_true', help='Opt in to triangle-driven atomic prototype')
    parser.add_argument('--edge-routing', action='store_true')
    args = parser.parse_args()
    if args.edge_routing and not args.triangles: parser.error('--edge-routing requires --triangles')
    work, asset, exe = args.work.resolve(), args.asset.resolve(), args.exe.resolve()
    work.mkdir(parents=True, exist_ok=False)
    result = {'complete': False, 'executable_sha256': sha256(exe), 'asset_sha256': sha256(asset),
              'shader_bundle_sha256': shader_bundle_sha256(exe.parent / 'shaders'), 'triangle_prototype': args.triangles, 'edge_routing': args.edge_routing, 'runs': []}
    output = work / 'results.json'
    for round_id in range(1, 4):
        for camera in ('far', 'near'):
            row = run_one(exe, asset, work, 'all_enabled', camera, round_id, 130, triangles=args.triangles, edge_routing=args.edge_routing)
            if row.get('gpu_frames') != 129 or not row.get('software_raster_active') or not row.get('hzb_cull_active'):
                raise ValueError(f'{row["tag"]}: incomplete timing or inactive feature')
            result['runs'].append(row)
            output.write_text(json.dumps(result, indent=2) + '\n')
    result['summary'] = {}
    for camera in ('far', 'near'):
        rows = [r for r in result['runs'] if r['camera'] == camera]
        result['summary'][camera] = {key: statistics.median(r[key] for r in rows)
            for key in ('gpu_ms', 'cpu_active_ms', 'cut_wall_us', 'software_raster_ms', 'visibility_raster_ms')}
    if sha256(exe) != result['executable_sha256'] or shader_bundle_sha256(exe.parent / 'shaders') != result['shader_bundle_sha256']:
        raise ValueError('executable or shader bundle changed during measurement')
    result['complete'] = True
    output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result['summary'], indent=2), flush=True)


if __name__ == '__main__':
    main()
