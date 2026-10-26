"""Independently recalculate measurements and check paired images and build identity."""
import csv
import hashlib
import json
import math
from pathlib import Path
import re
import statistics as st
import sys
from PIL import Image, ImageChops, ImageStat

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT/'scripts/measure'))
from measure_texture_residency_detailed import summarize_frames, percentile
BASE = ROOT/'tmp/residency-review-pinned-20261026'
STRESS = ROOT/'tmp/residency-review-pinned-stress-20261026'


def check_run(root, row, frames, warmup, degrees):
    run = root/row['tag']
    trace = list(csv.DictReader((run/'frames.csv').open()))
    assert len(trace) in (frames, frames+1), run
    assert [int(r['frame']) for r in trace] == list(range(1, len(trace)+1)), run
    assert all(float(b['unix_ms']) > float(a['unix_ms']) for a,b in zip(trace,trace[1:])), run
    calc = summarize_frames(trace[:frames], warmup)
    for key, value in calc.items():
        assert math.isclose(value, row[key], abs_tol=1e-7), (run,key,value,row[key])
    memory = [json.loads(s) for s in (run/'memory.jsonl').read_text().splitlines()]
    start, end = float(trace[warmup-1]['unix_ms']), float(trace[frames-1]['unix_ms'])
    steady = [r for r in memory if start <= r['unix_ms'] <= end]
    assert len(steady) >= 5
    for key in ('dedicated_bytes','shared_bytes','committed_bytes','working_set_bytes'):
        values = [r[key] for r in steady]
        assert all(v is not None and v >= 0 for v in values)
        assert row[key+'_median'] == st.median(values)
        assert row[key+'_peak'] == max(values)
        assert row[key+'_p95'] == percentile(values,.95)
        assert row[key+'_process_peak'] == max(r[key] for r in memory if r[key] is not None)
    log = (run/'deren-debug.log').read_text(errors='replace')
    assert not re.search(r'VUID-|Validation Error|SYNC-HAZARD|\[ERROR\]|post graph: recording refused|panic',log), run
    assert bool(row['residency_samples']) == row['selective'], run
    assert len(list(run.glob('screenshot_*.png'))) == 1
    config = (run/'config.toml').read_text()
    assert f'visibility_texture_streaming = {str(row["selective"]).lower()}' in config
    assert 'vsync = false' in config and 'max_fps = 0' in config and 'validation_layers = false' in config
    assert 'animation_time = 0.0' in config
    times = [float(r['interval_ms']) for r in trace[warmup:frames]]
    result = {k:row[k] for k in ('tag','fps','frame_mean_ms','frame_p95_ms','frame_p99_ms','frame_max_ms',
        'one_percent_low_fps','first_frame_ms','cold_max_ms','process_wall_seconds','dedicated_bytes_median',
        'dedicated_bytes_peak','dedicated_bytes_process_peak','shared_bytes_median','working_set_bytes_median')}
    result.update(frames_over_50_ms=sum(v>50 for v in times), frames_over_100_ms=sum(v>100 for v in times),
        memory_samples=len(steady), memory_sample_interval_ms=st.median([b['unix_ms']-a['unix_ms'] for a,b in zip(steady,steady[1:])]))
    samples = row['residency_samples']
    if samples:
        # Log records every upload, the first two updates, and every 60th stationary update.
        result['residency'] = dict(initial_update_ms=float(samples[0][5]),
            total_logged_upload_bytes=sum(int(s[3]) for s in samples),
            upload_event_count=sum(int(s[3])>0 for s in samples),
            max_logged_update_ms=max(float(s[5]) for s in samples),
            max_logged_deferred_unloads=max(int(s[4]) for s in samples),
            resident_payload_min=min(int(s[0]) for s in samples), resident_payload_max=max(int(s[0]) for s in samples))
    if frames >= 3600:
        revolutions=[]
        span=round(360/degrees)
        for revolution in range(frames//span):
            lo=max(warmup,revolution*span); hi=min(frames,(revolution+1)*span)
            a,b=float(trace[lo-1]['unix_ms']),float(trace[hi-1]['unix_ms'])
            mem=[r['dedicated_bytes'] for r in memory if a<=r['unix_ms']<=b]
            ram=[r['working_set_bytes'] for r in memory if a<=r['unix_ms']<=b]
            values=[float(r['interval_ms']) for r in trace[lo:hi]]
            revolutions.append(dict(revolution=revolution+1,first_frame=lo+1,last_frame=hi,
                dedicated_median_bytes=st.median(mem), dedicated_peak_bytes=max(mem),
                working_set_median_bytes=st.median(ram),working_set_peak_bytes=max(ram),
                fps=1000/st.mean(values),p99_ms=percentile(values,.99)))
        result['revolutions']=revolutions
    return result


def image_pair(first, second):
    paths=[next(p.glob('screenshot_*.png')) for p in (first,second)]
    images=[Image.open(p).convert('RGB') for p in paths]
    assert images[0].size == images[1].size == (1280,720)
    diff=ImageChops.difference(*images)
    equal=diff.getbbox() is None
    channels=diff.split()
    histogram=ImageChops.lighter(ImageChops.lighter(channels[0],channels[1]),channels[2]).histogram()
    return dict(eager=str(paths[0].relative_to(ROOT)),selective=str(paths[1].relative_to(ROOT)),
        rgb_identical=equal, changed_pixels=1280*720-histogram[0],
        pixels_over_1=sum(histogram[2:]),pixels_over_4=sum(histogram[5:]),
        max_channel_error=max(i for i,v in enumerate(histogram) if v),mean_absolute_rgb=ImageStat.Stat(diff).mean)


def main():
    manifest=json.loads((BASE/'manifest.json').read_text())
    identity=json.loads((BASE/'artifact-identity.json').read_text())
    for name,item in identity.items():
        if isinstance(item,dict): assert hashlib.sha256((ROOT/name).read_bytes()).hexdigest()==item['sha256'],name
    assert hashlib.sha256(Path(manifest['exe']).read_bytes()).hexdigest()==manifest['exe_sha256']
    rows=json.loads((BASE/'results.json').read_text())
    assert len(rows)==18
    assert {(r['round'],r['camera'],r['selective']) for r in rows} == {(n,c,s) for n in (1,2,3) for c in ('far','near','sweep') for s in (False,True)}
    baseline=[check_run(BASE,r,600,120,.5) for r in rows]
    pairs=[image_pair(BASE/f'r{n}-{c}-eager',BASE/f'r{n}-{c}-selective') for n in (1,2,3) for c in ('far','near','sweep')]
    repeat_checks=[]
    for camera in ('far','near','sweep'):
        for mode in ('eager','selective'):
            for n in (2,3):
                check=image_pair(BASE/f'r1-{camera}-{mode}',BASE/f'r{n}-{camera}-{mode}')
                check['camera']=camera;check['mode']=mode;check['second_round']=n
                repeat_checks.append(check)
    result=dict(build_identity_checked=True,baseline=baseline,pixel_pairs=pairs,same_mode_repeat_checks=repeat_checks)
    if '--baseline-only' not in sys.argv and (STRESS/'results.json').exists():
        stress_manifest=json.loads((STRESS/'manifest.json').read_text())
        assert stress_manifest['exe_sha256']==manifest['exe_sha256']
        stress_rows=json.loads((STRESS/'results.json').read_text())
        assert len(stress_rows)==8
        assert {(r['group'],r['round'],r['selective']) for r in stress_rows} == {('turnaround',n,s) for n in (1,2,3) for s in (False,True)} | {('long-loop',1,s) for s in (False,True)}
        result['stress']=[]
        for row in stress_rows:
            m=stress_manifest['groups'][row['group']]
            r=check_run(STRESS/row['group'],row,m['frames'],m['warmup'],m['sweep_degrees'])
            r['group']=row['group'];result['stress'].append(r)
        for group,rounds in (('turnaround',3),('long-loop',1)):
            pairs.extend(image_pair(STRESS/group/f'r{n}-sweep-eager',STRESS/group/f'r{n}-sweep-selective') for n in range(1,rounds+1))
        for frames in (1,2):
            pair=[STRESS/f'reveal-{frames}-{kind}' for kind in ('eager','selective')]
            for run in pair:
                s=json.loads((run/'summary.json').read_text());assert s['exit_code']==s['diagnostics']==0
                assert 'validation_layers = true' in (run/'config.toml').read_text()
            pairs.append(image_pair(*pair))
    result['exact_rgb_gate_passed']=all(p['rgb_identical'] for p in pairs)
    (BASE/'audit.json').write_text(json.dumps(result,indent=2))
    print(f'Measurement integrity passed: {len(baseline)} baseline runs, {len(result.get("stress",[]))} stress runs, build artifacts unchanged.')
    print(f'Exact RGB gate: {sum(p["rgb_identical"] for p in pairs)}/{len(pairs)} pairs identical; overall {result["exact_rgb_gate_passed"]}.')


if __name__=='__main__': main()
