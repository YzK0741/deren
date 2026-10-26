"""Same-build eager/selective captures. Payload bytes are not allocator/VRAM measurements."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import time
from analyze_object_lod import parse_log
from measure_render_optimizations import make_config, CAMERAS


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--asset', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--frames', type=int, default=64)
    args = parser.parse_args()
    exe, asset, work = args.exe.resolve(), args.asset.resolve(), args.out.resolve()
    work.mkdir(parents=True, exist_ok=True)
    rows = []
    for camera in ('far', 'near', 'sweep'):
        for enabled in (False, True):
            run = work / f'{camera}-{"selective" if enabled else "eager"}'
            run.mkdir(exist_ok=False)
            config = run / 'config.toml'
            make_config(config, asset, exe.parent / 'shaders', run, 'no_optimizations')
            config.write_text(config.read_text().replace('visibility_buffer = true',
                f'visibility_buffer = true\nvisibility_texture_streaming = {str(enabled).lower()}'), encoding='utf-8')
            env = {k: v for k, v in os.environ.items() if not k.startswith('DEREN_')}
            env['PATH'] = 'C:/msys64/clang64/bin;' + env.get('PATH', '')
            command = [str(exe), '--config', str(config), '--capture-frames', str(args.frames),
                f'--capture-camera={CAMERAS["near" if camera == "sweep" else camera]}']
            if camera == 'sweep': command += ['--capture-sweep', '2']
            started = time.perf_counter()
            with (run / 'stdout.log').open('wb') as output:
                process = subprocess.run(command, cwd=run, env=env, stdout=output, stderr=subprocess.STDOUT,
                    timeout=600, creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
            log = (run / 'deren-debug.log').read_text(encoding='utf-8', errors='replace')
            if process.returncode or not list(run.glob('screenshot_*.png')) or re.search(r'VUID-|Validation Error|post graph: recording refused|panic', log):
                raise RuntimeError(f'{run}: renderer/capture failure')
            row = dict(camera=camera, selective=enabled, process_wall_seconds=time.perf_counter() - started, **parse_log(log))
            values = re.findall(r'texture residency: resident payload (\d+) / (\d+) bytes, pinned (\d+) bytes, new demand (\d+) bytes, deferred unload (\d+), prefetched (\d+), waited (\d+), update ([0-9.eE+-]+) ms', log)
            if enabled and not values: raise RuntimeError(f'{run}: no residency evidence')
            row['residency_samples'] = [dict(zip(('resident_bytes','total_bytes','pinned_bytes','upload_bytes','deferred_unloads','prefetched','waited','update_ms'),
                [int(v) for v in sample[:7]] + [float(sample[7])], strict=True)) for sample in values]
            rows.append(row)
            (work / 'results.json').write_text(json.dumps(rows, indent=2), encoding='utf-8')
            print(json.dumps(row), flush=True)


if __name__ == '__main__': main()
