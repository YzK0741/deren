"""Repeated Bistro FPS and per-process WDDM memory comparison. Run with GPU-counter access."""
import argparse
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import re
import statistics
import subprocess
import time
from measure_render_optimizations import make_config, CAMERAS


def percentile(values, fraction):
    values = sorted(values)
    position = (len(values)-1)*fraction
    low = math.floor(position)
    return values[low] + (values[math.ceil(position)]-values[low])*(position-low)


def summarize_frames(rows, warmup):
    if len(rows) <= warmup: raise ValueError('too few frames')
    times = [float(row['interval_ms']) for row in rows[warmup:]]
    if any(not math.isfinite(t) or t <= 0 for t in times): raise ValueError('invalid frame interval')
    slowest = sorted(times, reverse=True)[:max(1, math.ceil(len(times)*.01))]
    return dict(frame_count=len(times), fps=1000/statistics.mean(times), frame_mean_ms=statistics.mean(times),
        frame_median_ms=statistics.median(times), frame_p95_ms=percentile(times,.95), frame_p99_ms=percentile(times,.99),
        frame_max_ms=max(times), one_percent_low_fps=1000/statistics.mean(slowest),
        first_frame_ms=float(rows[0]['interval_ms']), cold_max_ms=max(float(r['interval_ms']) for r in rows[:warmup]))


def gpu_snapshot():
    result = subprocess.run(['nvidia-smi','--query-gpu=name,driver_version,memory.total,memory.used,temperature.gpu,clocks.gr,power.draw,pstate',
        '--format=csv,noheader'], capture_output=True, text=True, check=True, creationflags=subprocess.CREATE_NO_WINDOW)
    return result.stdout.strip()


def run_one(args, camera, enabled, round_id):
    tag = f'r{round_id}-{camera}-{"selective" if enabled else "eager"}'
    run = args.out / tag
    run.mkdir()
    config = run/'config.toml'
    make_config(config, args.asset, args.exe.parent/'shaders', run, 'no_optimizations')
    config.write_text(config.read_text().replace('visibility_buffer = true',
        f'visibility_buffer = true\nvisibility_texture_streaming = {str(enabled).lower()}'), encoding='utf-8')
    environment = {k:v for k,v in os.environ.items() if not k.startswith('DEREN_')}
    environment['PATH'] = 'C:/msys64/clang64/bin;' + environment.get('PATH','')
    environment['DEREN_BENCH_FRAME_TRACE'] = str(run/'frames.csv')
    command = [str(args.exe),'--config',str(config),'--capture-frames',str(args.frames),
        f'--capture-camera={CAMERAS["near" if camera == "sweep" else camera]}']
    if camera == 'sweep': command += ['--capture-sweep',str(args.sweep_degrees)]
    before = gpu_snapshot()
    started = time.perf_counter()
    with (run/'stdout.log').open('wb') as output, (run/'sampler.log').open('wb') as sampler_output:
        process = subprocess.Popen(command, cwd=run, env=environment, stdout=output, stderr=subprocess.STDOUT,
            creationflags=subprocess.CREATE_NO_WINDOW)
        sampler = subprocess.Popen(['powershell.exe','-NoProfile','-ExecutionPolicy','Bypass','-File',str(args.sampler),
            '-TargetProcessId',str(process.pid),'-OutFile',str(run/'memory.jsonl')],
            stdout=sampler_output, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            result = process.wait(timeout=900)
            sampler_result = sampler.wait(timeout=30)
        except BaseException:
            if process.poll() is None: process.kill()
            if sampler.poll() is None: sampler.kill()
            raise
    after = gpu_snapshot()
    log = (run/'deren-debug.log').read_text(encoding='utf-8',errors='replace')
    if result or sampler_result or re.search(r'VUID-|Validation Error|post graph: recording refused|panic',log):
        raise RuntimeError(f'{tag}: renderer/sampler failed; inspect logs')
    with (run/'frames.csv').open() as stream: frames = list(csv.DictReader(stream))
    # Screenshot requests are consumed on the following frame in the current capture protocol.
    observed_frames = len(frames)
    if observed_frames not in (args.frames, args.frames+1):
        raise ValueError(f'{tag}: expected {args.frames} (+ optional capture frame), got {observed_frames}')
    frames = frames[:args.frames] # exclude the final capture/teardown frame from throughput
    stats = summarize_frames(frames,args.warmup)
    start, finish = float(frames[args.warmup-1]['unix_ms']),float(frames[-1]['unix_ms'])
    memory = [json.loads(line) for line in (run/'memory.jsonl').read_text().splitlines()]
    steady = [row for row in memory if start <= row['unix_ms'] <= finish]
    if len(steady) < 5: raise ValueError(f'{tag}: only {len(steady)} steady memory samples')
    for key in ('dedicated_bytes','shared_bytes','committed_bytes','working_set_bytes'):
        values = [r[key] for r in steady]
        stats[key+'_median'] = statistics.median(values)
        stats[key+'_p95'] = percentile(values,.95)
        stats[key+'_peak'] = max(values)
        stats[key+'_process_peak'] = max(r[key] for r in memory if r[key] is not None)
    stats['memory_samples'] = len(steady)
    stats['observed_frames'] = observed_frames
    residency = re.findall(r'texture residency: resident payload (\d+) / (\d+) bytes, pinned (\d+) bytes, new demand (\d+) bytes, deferred unload (\d+), update ([0-9.eE+-]+) ms',log)
    stats['residency_samples'] = residency
    stats.update(tag=tag,camera=camera,selective=enabled,round=round_id,
        process_wall_seconds=time.perf_counter()-started,gpu_before=before,gpu_after=after,
        steady_start_unix_ms=start,steady_end_unix_ms=finish)
    (run/'summary.json').write_text(json.dumps(stats,indent=2))
    print(f'{tag}: {stats["fps"]:.2f} FPS; dedicated {stats["dedicated_bytes_median"]/2**20:.1f} MiB; p99 {stats["frame_p99_ms"]:.2f} ms',flush=True)
    return stats


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe',type=Path,required=True)
    parser.add_argument('--asset',type=Path,required=True)
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--rounds',type=int,default=3)
    parser.add_argument('--frames',type=int,default=600)
    parser.add_argument('--warmup',type=int,default=120)
    parser.add_argument('--sweep-degrees',type=float,default=.5)
    args = parser.parse_args()
    if not 1 <= args.warmup < args.frames: parser.error('warmup must be within the frame range')
    args.exe,args.asset,args.out = args.exe.resolve(),args.asset.resolve(),args.out.resolve()
    args.sampler = Path(__file__).with_name('sample_gpu_process_memory.ps1').resolve()
    args.out.mkdir(parents=True,exist_ok=False)
    manifest = dict(exe=str(args.exe),asset=str(args.asset),frames=args.frames,warmup=args.warmup,rounds=args.rounds,
        sweep_degrees=args.sweep_degrees,exe_sha256=hashlib.sha256(args.exe.read_bytes()).hexdigest(),gpu=gpu_snapshot())
    (args.out/'manifest.json').write_text(json.dumps(manifest,indent=2))
    rows=[]
    for round_id in range(1,args.rounds+1):
        cameras = ('far','near','sweep') if round_id%2 else ('sweep','near','far')
        for camera in cameras:
            for enabled in ((False,True) if round_id%2 else (True,False)):
                rows.append(run_one(args,camera,enabled,round_id))
                (args.out/'results.json').write_text(json.dumps(rows,indent=2))


if __name__ == '__main__': main()
