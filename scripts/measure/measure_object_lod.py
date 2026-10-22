"""Run interleaved real-renderer object LOD captures and keep every raw run.

The import-ready wall metric includes process/device/environment setup; it is
sampled at the first completed scene-import log, with <=50ms polling granularity.
Peak working set is a process CPU memory measure, not GPU allocation size.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time

from analyze_object_lod import summarize, parse_log

CAMERAS = {'far': '35,18,140,0,-83.07,0', 'near': '35,8,42,0,-88,0'}


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for b in iter(lambda: f.read(1024 * 1024), b''):
            h.update(b)
    return h.hexdigest()


def peak_memory(process):
    if os.name != 'nt':
        return None
    class Counters(ctypes.Structure):
        _fields_ = [('cb', ctypes.c_ulong), ('faults', ctypes.c_ulong)] + [
            (key, ctypes.c_size_t) for key in ('peak', 'working', 'paged_peak', 'paged',
                                              'nonpaged_peak', 'nonpaged', 'pagefile', 'pagefile_peak')]
    counters = Counters()
    counters.cb = ctypes.sizeof(counters)
    query = ctypes.windll.psapi.GetProcessMemoryInfo
    query.argtypes = [ctypes.c_void_p, ctypes.POINTER(Counters), ctypes.c_ulong]
    query.restype = ctypes.c_int
    return counters.peak if query(int(process._handle), ctypes.byref(counters), counters.cb) else None



def capture(exe, asset, shaders, work, tag, camera, validation, on, frames=130, probe=False):
    run = work / tag
    run.mkdir(parents=True, exist_ok=False)
    config = run / 'config.toml'
    q = lambda p: json.dumps(str(p).replace('\\', '/'))
    config.write_text(f'''model = {q(asset)}
[paths]
shaders_dir = {q(shaders)}
screenshot_dir = {q(run)}
[render]
window_width = 1280
window_height = 720
vsync = false
max_fps = 0
validation_layers = {str(validation).lower()}
visibility_buffer = true
subtilis = {str(on).lower()}
subtilis_error_px = 1.0
subtilis_hysteresis = false
character_forward = false
shadow = false
taa = false
ssao = false
bloom = false
fxaa = false
gpu_timings = true
[gui]
show = false
[lighting]
env_size = 128
env_mip_count = 4
irr_size = 16
lut_size = 128
''', encoding='utf-8')
    env = os.environ.copy()
    for key in list(env):
        if key.startswith('DEREN_'):
            del env[key]
    if os.name == 'nt':
        env['PATH'] = 'C:/msys64/clang64/bin;' + env.get('PATH', '')
    if probe:
        env['DEREN_CLUSTER_PROBE_DUMP'] = str(run / 'clusters.bin')
        env['DEREN_DEPTH_PROBE_CAPTURE'] = str(run / 'depth.bin')
    command = [str(exe), '--config', str(config), '--capture-frames', str(frames), '--capture-camera='+CAMERAS[camera]]
    started = time.perf_counter()
    ready, peak = None, None
    with (run / 'stdout.log').open('wb') as stdout:
        process = subprocess.Popen(command, cwd=run, env=env, stdout=stdout, stderr=subprocess.STDOUT,
                                   creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        while process.poll() is None:
            memory = peak_memory(process)
            if memory is not None:
                peak = max(peak or 0, memory)
            log = run / 'debug.log'
            if ready is None and log.exists():
                text = log.read_text(encoding='utf-8', errors='replace')
                if re.search(r'imported \d+ primitives', text):
                    ready = time.perf_counter() - started
            if time.perf_counter() - started > 600:
                process.kill()
                process.wait()
                raise TimeoutError(tag)
            time.sleep(.05)
    elapsed = time.perf_counter() - started
    log = run / 'debug.log'
    text = log.read_text(encoding='utf-8', errors='replace') if log.exists() else ''
    images = list(run.glob('screenshot_*.png'))
    result = {'tag': tag, 'camera': camera, 'validation': validation, 'on': on, 'frames': frames,
              'exit_code': process.returncode, 'process_wall_seconds': elapsed,
              'import_ready_seconds': ready, 'peak_working_set_bytes': peak,
              'log': str(log), 'image': str(images[0]) if len(images) == 1 else None,
              'command': command, **parse_log(text)}
    (run / 'run.json').write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    if process.returncode or not result['image'] or not result['validation_clean']:
        raise RuntimeError(f'failed capture: {tag}; inspect {log}')
    print(f'{tag}: {elapsed:.2f}s, GPU {result.get("gpu_ms")}ms', flush=True)
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--asset', type=Path, default=Path('D:/deren-assets/2026-10-21-object-lod/bistro-object-lod.glb'))
    p.add_argument('--exe', type=Path, default=Path('build-release-dyn-clang64/deren.exe'))
    p.add_argument('--work', type=Path, default=Path('D:/deren-assets/2026-10-21-object-lod/runtime'))
    p.add_argument('--rounds', type=int, default=3)
    p.add_argument('--smoke', action='store_true', help='one ON run, not an accepted measurement matrix')
    a = p.parse_args()
    if a.rounds < 3:
        p.error('formal measurements require at least three rounds')
    exe, asset, work = a.exe.resolve(), a.asset.resolve(), a.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    data = {'rounds': a.rounds, 'asset': str(asset), 'asset_sha256': sha(asset),
            'exe': str(exe), 'exe_sha256': sha(exe), 'camera_parameters': CAMERAS, 'runs': [], 'load_runs': [], 'probes': []}
    if a.smoke:
        capture(exe, asset, exe.parent/'shaders', work, 'smoke-on', 'far', True, True)
        return
    def save():
        (work / 'measurements.json').write_text(json.dumps(data, indent=2)+'\n', encoding='utf-8')
    for validation in (True, False):
        for camera in CAMERAS:
            for r in range(1, a.rounds+1):
                for on in ((False, True) if r % 2 else (True, False)):
                    tag = f'{camera}-val{int(validation)}-on{int(on)}-{r}'
                    row = capture(exe, asset, exe.parent/'shaders', work, tag, camera, validation, on)
                    row['round'] = r
                    data['runs'].append(row)
                    save()
    for r in range(1, 4):
        for on in (False, True):
            data['load_runs'].append(capture(exe, asset, exe.parent/'shaders', work,
                                            f'load-on{int(on)}-{r}', 'far', False, on, frames=3))
            save()
    for camera in CAMERAS:
        data['probes'].append(capture(exe, asset, exe.parent/'shaders', work,
                                     f'probe-{camera}', camera, True, True, probe=True))
        save()
    data['summary'] = summarize(data)
    save()
    print('complete matrix validated')


if __name__ == '__main__':
    main()
