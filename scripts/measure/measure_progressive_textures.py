"""Serial GPU captures for two-tier residency; keep raw traces and commands."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import time


def memory_evidence_error(summary):
    if summary.get('memory_sampler_exit') != 0:
        return 'memory sampler failed; inspect memory-sampler.log and CIM permissions'
    if summary.get('memory_steady_samples', 0) < 3 or 'dedicated_steady_median_bytes' not in summary:
        return 'not enough steady memory counters; use --frames 900 and check sampler output'
    return None


def summarize_resources(rows):
    if not rows or any(row.get('supported') != '1' for row in rows):
        raise ValueError('missing or unsupported resource statistics')
    columns = ['frame', 'gpu_buffer_local', 'gpu_image_local', 'gpu_total_local', 'gpu_peak_local',
               'gpu_total', 'gpu_reserved', 'gpu_reserved_local', 'cpu_current', 'cpu_peak']
    try:
        rows = [{column: int(row[column]) for column in columns} for row in rows]
    except (KeyError, TypeError, ValueError) as error:
        raise ValueError('missing or malformed resource counters') from error
    previous = None
    for row in rows:
        if any(value < 0 for value in row.values()):
            raise ValueError('negative resource counter')
        if row['gpu_buffer_local'] + row['gpu_image_local'] != row['gpu_total_local'] or row['gpu_total'] < row['gpu_total_local']:
            raise ValueError('inconsistent resource totals')
        if row['gpu_peak_local'] < row['gpu_total_local'] or row['cpu_peak'] < row['cpu_current']:
            raise ValueError('resource peak below current usage')
        if previous is not None and (row['frame'] <= previous['frame'] or
                row['gpu_peak_local'] < previous['gpu_peak_local'] or row['cpu_peak'] < previous['cpu_peak']):
            raise ValueError('nonmonotonic resource samples')
        previous = row
    steady = [row for row in rows if int(row['frame']) >= 180]
    if len(steady) < 3:
        raise ValueError('insufficient steady resource samples')
    summary = dict(samples=len(rows), steady_samples=len(steady))
    for column in ['gpu_buffer_local', 'gpu_image_local', 'gpu_total_local', 'gpu_total',
                   'gpu_reserved', 'gpu_reserved_local', 'cpu_current']:
        values = [int(row[column]) for row in steady]
        summary[column + '_steady_median_bytes'] = statistics.median(values)
        summary[column + '_steady_max_bytes'] = max(values)
    for column in ['gpu_peak_local', 'cpu_peak']:
        summary[column + '_bytes'] = max(int(row[column]) for row in rows)
    return summary


def feature_activity_errors(metrics, *, subtilis=False, hzb=False, software=False, gpu_software=False):
    required = []
    if subtilis:
        required += ['cluster_dag_active', 'adopted']
    if hzb:
        required += ['hzb_cull_active', 'hzb_recovery_active']
    if software:
        required += ['software_raster_active']
    if gpu_software:
        required += ['software_triangle_active', 'software_gpu_routing_active']
    return [f'requested feature did not execute: {field}' for field in required if not metrics.get(field)]


def summarize_process_memory(samples, frames):
    if len(frames) <= 180:
        raise ValueError('not enough frame timestamps for process memory')
    start, end = float(frames[180]['unix_ms']), float(frames[-1]['unix_ms'])
    steady = [row for row in samples if start <= float(row['unix_ms']) <= end]
    if len(steady) < 3:
        raise ValueError('insufficient steady process memory samples')
    for row in steady:
        if int(row['working_bytes']) <= 0 or int(row['private_bytes']) <= 0 or int(row['peak_working_bytes']) < int(row['working_bytes']):
            raise ValueError('invalid process memory counters')
    result = dict(samples=len(samples), steady_samples=len(steady),
                  working_peak_bytes=max(int(row['peak_working_bytes']) for row in steady))
    for key in ['working', 'private']:
        values = [int(row[key + '_bytes']) for row in steady]
        result[key + '_steady_median_bytes'] = statistics.median(values)
        result[key + '_steady_max_bytes'] = max(values)
    return result


def load_process_memory_summary(memory_file, frame_file):
    if not frame_file.is_file():
        raise ValueError('process memory requires a frame timestamp trace')
    with frame_file.open() as file:
        frames = list(csv.DictReader(file))
    samples = [json.loads(line) for line in memory_file.read_text().splitlines()]
    return summarize_process_memory(samples, frames)


def make_process_memory_reader(process):
    # PROCESS_MEMORY_COUNTERS_EX layout matches the local Windows psapi.h; no CIM service required.
    import ctypes
    class Counters(ctypes.Structure):
        _fields_ = [('cb', ctypes.c_ulong), ('faults', ctypes.c_ulong)] + [
            (key, ctypes.c_size_t) for key in ('peak', 'working', 'paged_peak', 'paged',
                                             'nonpaged_peak', 'nonpaged', 'pagefile', 'pagefile_peak', 'private')]
    query = ctypes.WinDLL('psapi', use_last_error=True).GetProcessMemoryInfo
    query.argtypes = [ctypes.c_void_p, ctypes.POINTER(Counters), ctypes.c_ulong]
    query.restype = ctypes.c_int
    def read():
        counters = Counters()
        counters.cb = ctypes.sizeof(counters)
        if not query(int(process._handle), ctypes.byref(counters), counters.cb):
            if process.poll() is not None:
                return None
            raise OSError(ctypes.get_last_error(), 'GetProcessMemoryInfo failed')
        return dict(unix_ms=time.time()*1000, working_bytes=counters.working,
                    private_bytes=counters.private, peak_working_bytes=counters.peak)
    return read


def wait_with_process_memory(process):
    samples = []
    deadline = time.monotonic() + 300
    try:
        read = make_process_memory_reader(process)
        while process.poll() is None:
            if time.monotonic() >= deadline:
                raise subprocess.TimeoutExpired(process.args, 300)
            sample = read()
            if sample is None:
                break
            samples.append(sample)
            try:
                process.wait(timeout=0.1)
            except subprocess.TimeoutExpired:
                pass
        return process.wait(), samples
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--frames', type=int, default=360)
    parser.add_argument('--sweep', type=float, default=0)
    parser.add_argument('--budget', type=int, default=512)
    parser.add_argument('--mode', choices=['progressive', 'gpu', 'eager'], default='progressive')
    parser.add_argument('--model', type=Path, default=Path('D:/deren-assets/bistro/textured-verified/bistro-core-mr.gltf'))
    parser.add_argument('--validation', action='store_true')
    parser.add_argument('--disable-streaming-frame', type=int, default=-1)
    parser.add_argument('--mips', action='store_true')
    parser.add_argument('--software', action='store_true')
    parser.add_argument('--subtilis', action='store_true', help='Enable imported geometry DAG LOD independently of software raster')
    parser.add_argument('--hzb', action='store_true', help='Enable HZB build, cull and recovery')
    parser.add_argument('--gpu-software', action='store_true', help='Experimental triangle raster and GPU survivor routing; requires --software --hzb')
    parser.add_argument('--operation-profile', action='store_true', help='Diagnostic CPU attribution; exclude these frame timings from performance comparisons')
    parser.add_argument('--record-trace', action='store_true', help='Per-pass graph-record trace; exclude these frame timings from performance comparisons')
    parser.add_argument('--parallel-visibility', action='store_true', help='Record visibility item ranges into per-worker mergeable segments; diagnostic')
    parser.add_argument('--source-levels', type=int, default=0)
    parser.add_argument('--camera', default='35,8,42,0,-88,0')
    parser.add_argument('--memory', action='store_true', help='Separate WDDM memory run; do not use its timings for performance comparisons')
    parser.add_argument('--resource-memory', action='store_true', help='Separate resource/vessel counter capture; exclude its frame timings')
    parser.add_argument('--process-memory', action='store_true', help='Also sample Windows working set and private bytes; requires --resource-memory')
    parser.add_argument('--unlit', action='store_true')
    parser.add_argument('--no-visibility', action='store_true')
    args = parser.parse_args()
    if args.process_memory and (os.name != 'nt' or not args.resource_memory or args.memory):
        parser.error('--process-memory requires Windows --resource-memory, without --memory')
    if args.gpu_software and not (args.software and args.hzb):
        parser.error('--gpu-software requires --software --hzb')
    if args.no_visibility and (args.subtilis or args.software or args.hzb):
        parser.error('geometry optimization captures require visibility rendering')
    if (args.memory or args.resource_memory) and args.frames < 240:
        parser.error('memory captures need at least 240 frames; use --frames 900')
    out, exe = args.out.resolve(), args.exe.resolve()
    out.mkdir(parents=True, exist_ok=False)
    template = f'''model = "{args.model.resolve().as_posix()}"
[paths]
shaders_dir = "{(exe.parent/'shaders').as_posix()}"
screenshot_dir = "{out.as_posix()}"
[render]
animation_time = 0.0
window_width = 1280
window_height = 720
vsync = false
max_fps = 0
validation_layers = {str(args.validation).lower()}
main_render_graph = true
visibility_buffer = {str(not args.no_visibility).lower()}
unlit = {str(args.unlit).lower()}
visibility_texture_streaming = {str(args.mode != 'eager').lower()}
subtilis = {str(args.subtilis or args.software).lower()}
subtilis_error_px = 1.0
subtilis_hysteresis = false
software_raster = {str(args.software).lower()}
software_raster_threshold_px = 1.0
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
'''
    config = out/'config.toml'
    config.write_text(template)
    env = os.environ.copy()
    for key in list(env):
        if key.startswith('DEREN_'):
            del env[key]
    env['DEREN_BENCH_FRAME_TRACE'] = str(out/'frames.csv')
    env.pop('DEREN_BENCH_RESOURCE_TRACE', None)
    if args.resource_memory:
        env['DEREN_BENCH_RESOURCE_TRACE'] = str(out/'resources.csv')
    env['DEREN_BENCH_DISABLE_STREAMING_FRAME'] = str(args.disable_streaming_frame)
    env.pop('DEREN_BENCH_TEXTURE_LEVELS', None)
    if args.source_levels:
        env['DEREN_BENCH_TEXTURE_LEVELS'] = str(args.source_levels)
    if args.mode == 'progressive':
        env.update(DEREN_PROGRESSIVE_TEXTURES='1', DEREN_PROGRESSIVE_BUDGET_MB=str(args.budget),
                   DEREN_PROGRESSIVE_TRACE=str(out/'residency.csv'))
    if args.mips:
        env['DEREN_PROGRESSIVE_MIPS'] = '1'
    if args.hzb:
        env.update(DEREN_HZB_BUILD='1', DEREN_HZB_CULL='1', DEREN_HZB_RECOVERY='1')
    if args.gpu_software:
        env.update(DEREN_SOFTWARE_RASTER_TRIANGLES='1', DEREN_SOFTWARE_RASTER_GPU='1')
    if args.operation_profile:
        env['DEREN_RENDER_OPERATION_PROFILE'] = '1'
    if args.record_trace:
        env['DEREN_RECORD_TRACE'] = '1'
    if args.parallel_visibility:
        env['DEREN_PARALLEL_VISIBILITY'] = '1'
    cmd = [str(exe), '--config', str(config), '--capture-frames', str(args.frames),
           '--capture-camera='+args.camera, '--capture-sweep', str(args.sweep)]
    hashes = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in [exe, *exe.parent.glob('*.dll')]}
    shader_hashes = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in (exe.parent/'shaders').glob('*.spv')}
    (out/'manifest.json').write_text(json.dumps(dict(command=cmd, mode=args.mode, budget_mb=args.budget,
        validation=args.validation, mips=args.mips, software=args.software, subtilis=args.subtilis or args.software,
        hzb=args.hzb, gpu_software=args.gpu_software, operation_profile=args.operation_profile, record_trace=args.record_trace, parallel_visibility=args.parallel_visibility, source_levels=args.source_levels, memory=args.memory,
        resource_memory=args.resource_memory, process_memory=args.process_memory, hashes=hashes, shader_hashes=shader_hashes,
        model_sha256=hashlib.sha256(args.model.read_bytes()).hexdigest(),
        disable_streaming_frame=args.disable_streaming_frame, unlit=args.unlit, no_visibility=args.no_visibility,
        environment={key: value for key, value in env.items() if key.startswith('DEREN_')}), indent=2))
    sampler_exit = None
    with (out/'run.log').open('w') as log:
        if args.memory:
            with (out/'memory-sampler.log').open('w') as memory_log:
                process = subprocess.Popen(cmd, cwd=out, env=env, stdout=log, stderr=subprocess.STDOUT)
                sampler = subprocess.Popen(['powershell', '-NoProfile', '-WindowStyle', 'Hidden', '-ExecutionPolicy', 'Bypass',
                    '-File', str(Path(__file__).with_name('sample_gpu_memory.ps1').resolve()),
                    '-TargetProcessId', str(process.pid), '-OutputPath', str(out/'memory.jsonl')],
                    stdout=memory_log, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
                try:
                    status = process.wait(timeout=300)
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.wait()
                    try:
                        sampler_exit = sampler.wait(timeout=15)
                    except subprocess.TimeoutExpired:
                        sampler.terminate()
                        sampler_exit = sampler.wait()
                result = subprocess.CompletedProcess(cmd, status)
        elif args.process_memory:
            process = subprocess.Popen(cmd, cwd=out, env=env, stdout=log, stderr=subprocess.STDOUT)
            status, samples = wait_with_process_memory(process)
            (out/'process-memory.jsonl').write_text(''.join(json.dumps(row) + '\n' for row in samples))
            result = subprocess.CompletedProcess(cmd, status)
        else:
            result = subprocess.run(cmd, cwd=out, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=300)
    logs = '\n'.join(p.read_text(errors='replace') for p in out.glob('*.log'))
    errors = len(re.findall(r'VUID-|\[ERROR\]|Validation Error', logs))
    summary = dict(exit_code=result.returncode, errors=errors, screenshots=len(list(out.glob('screenshot_*.png'))))
    if args.subtilis or args.software or args.hzb:
        from measure_render_optimizations import parse_run_log
        metrics = parse_run_log(logs)
        metrics['software_triangle_active'] = 'software triangle prototype: recorded clear/raster/unpack' in logs
        metrics['software_gpu_routing_active'] = 'software GPU routing active: HZB compaction, bounded software list' in logs
        summary['features'] = metrics
        summary['feature_errors'] = feature_activity_errors(metrics, subtilis=args.subtilis or args.software,
            hzb=args.hzb, software=args.software, gpu_software=args.gpu_software)
    if args.resource_memory:
        with (out/'resources.csv').open() as resource_file:
            summary['resources'] = summarize_resources(list(csv.DictReader(resource_file)))
    trace = out/'residency.csv'
    if trace.exists():
        rows = list(csv.DictReader(trace.open()))
        summary.update(updates=len(rows), final=rows[-1] if rows else None,
            max_jobs=max((int(r['jobs']) for r in rows), default=0),
            max_high_bytes=max((int(r['high_bytes']) for r in rows), default=0),
            max_pending_bytes=max((int(r['pending_bytes']) for r in rows), default=0),
            uploads=sum(int(r['started']) for r in rows),
            failed=max((int(r['failed']) for r in rows), default=0))
    frame_file = out/'frames.csv'
    if args.process_memory:
        summary['process_memory'] = load_process_memory_summary(out/'process-memory.jsonl', frame_file)
    if frame_file.exists():
        frame_rows = list(csv.DictReader(frame_file.open()))
        summary['frame_columns'] = list(frame_rows[0]) if frame_rows else []
        if frame_rows and 'interval_ms' in frame_rows[0]:
            times = [float(r['interval_ms']) for r in frame_rows[60:]]
            if times:
                summary.update(mean_ms=statistics.mean(times), p95_ms=sorted(times)[int((len(times)-1)*.95)])
        if args.memory:
            memory_path = out/'memory.jsonl'
            memory = [json.loads(line) for line in memory_path.read_text(encoding='utf-8-sig').splitlines()] if memory_path.exists() else []
            steady = [r for r in memory if len(frame_rows) > 180 and
                      float(frame_rows[180]['unix_ms']) <= r['unix_ms'] <= float(frame_rows[-1]['unix_ms'])]
            summary.update(memory_sampler_exit=sampler_exit, memory_samples=len(memory), memory_steady_samples=len(steady))
            if steady:
                summary.update(dedicated_steady_median_bytes=statistics.median(r['dedicated_bytes'] for r in steady),
                    dedicated_steady_peak_bytes=max(r['dedicated_bytes'] for r in steady),
                    shared_steady_median_bytes=statistics.median(r['shared_bytes'] for r in steady))
    memory_error = memory_evidence_error(summary) if args.memory else None
    if args.memory:
        summary['memory_valid'] = memory_error is None
        if memory_error:
            summary['memory_error'] = memory_error
    (out/'summary.json').write_text(json.dumps(summary, indent=2))
    print(json.dumps(summary, indent=2))
    if memory_error:
        raise SystemExit(memory_error)
    if result.returncode or errors or summary['screenshots'] != 1 or summary.get('failed', 0) or summary.get('feature_errors'):
        raise SystemExit(1)


if __name__ == '__main__':
    main()
