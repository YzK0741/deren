"""Reuse the existing benchmark harness for approved camera stress cases."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / 'scripts/measure'))
from measure_texture_residency_detailed import run_one, gpu_snapshot
from measure_render_optimizations import make_config as original_make_config, CAMERAS
import measure_texture_residency_detailed as harness


def make_config(*args):
    original_make_config(*args)
    path = args[0]
    path.write_text(path.read_text().replace('[render]\n', '[render]\nanimation_time = 0.0\n'))


harness.make_config = make_config

out = ROOT / 'tmp/residency-review-pinned-stress-20261026'
out.mkdir(exist_ok=False)
exe = ROOT / 'build-release-dyn-clang64/deren.exe'
asset = Path('D:/deren-assets/bistro/textured-verified/bistro-core-mr.gltf')
manifest = dict(exe=str(exe), exe_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
                asset=str(asset), gpu=gpu_snapshot(), groups={})
rows = []
for name, frames, warmup, degrees, rounds in (
    ('turnaround', 360, 60, 180, 3),
    ('long-loop', 3600, 120, .5, 1),
):
    group = out / name
    group.mkdir()
    manifest['groups'][name] = dict(frames=frames, warmup=warmup, sweep_degrees=degrees, rounds=rounds)
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2))
    args = SimpleNamespace(exe=exe, asset=asset, out=group, frames=frames, warmup=warmup,
                           sweep_degrees=degrees, sampler=ROOT/'scripts/measure/sample_gpu_process_memory.ps1')
    for round_id in range(1, rounds+1):
        for enabled in ((False, True) if round_id % 2 else (True, False)):
            row = run_one(args, 'sweep', enabled, round_id)
            row['group'] = name
            rows.append(row)
            (out / 'results.json').write_text(json.dumps(rows, indent=2))

# Small validation-enabled captures immediately after the first turnaround and return.
for frames in (1, 2):
    for enabled in (False, True):
        run = out / f'reveal-{frames}-{"selective" if enabled else "eager"}'
        run.mkdir()
        config = run / 'config.toml'
        make_config(config, asset, exe.parent/'shaders', run, 'no_optimizations')
        config.write_text(config.read_text().replace('validation_layers = false', 'validation_layers = true')
            .replace('visibility_buffer = true', f'visibility_buffer = true\nvisibility_texture_streaming = {str(enabled).lower()}'))
        env = {k:v for k,v in os.environ.items() if not k.startswith('DEREN_')}
        env['PATH'] = 'C:/msys64/clang64/bin;' + env.get('PATH', '')
        with (run/'stdout.log').open('wb') as stream:
            completed = subprocess.run([str(exe), '--config', str(config), '--capture-frames', str(frames),
                f'--capture-camera={CAMERAS["near"]}', '--capture-sweep', '180'], cwd=run, env=env,
                stdout=stream, stderr=subprocess.STDOUT, timeout=900, creationflags=subprocess.CREATE_NO_WINDOW)
        log = (run/'deren-debug.log').read_text(errors='replace')
        errors = re.findall(r'VUID-|Validation Error|SYNC-HAZARD|\[ERROR\]|post graph: recording refused|panic', log)
        result = dict(requested_frames=frames, selective=enabled, exit_code=completed.returncode,
                      diagnostics=len(errors), screenshots=len(list(run.glob('screenshot_*.png'))))
        (run/'summary.json').write_text(json.dumps(result, indent=2))
        if completed.returncode or errors or result['screenshots'] != 1:
            raise RuntimeError(f'{run}: validation capture failure: {result}')
        print(f'{run.name}: validation clean, capture ready', flush=True)

assert hashlib.sha256(exe.read_bytes()).hexdigest() == manifest['exe_sha256']
print('Stress captures complete', flush=True)
