import csv, gzip, hashlib, json, re
from pathlib import Path
import numpy as np
from PIL import Image

repo = Path('C:/Users/23530/Desktop/yzk/deren')
build = repo / 'build-release-dyn-clang64'
work = build / 'async-present-check'
out = repo / 'docs/benchmarks/2026-10-12-async-present'
out.mkdir(parents=True, exist_ok=True)
result = {'method': {'warmup_rows': 60, 'selected_rows': [61, 360], 'repetitions': 3,
    'order': ['baseline/candidate', 'candidate/baseline', 'baseline/candidate'],
    'cpu_phase_definition': 'median of 15 rounded 60-frame window means, not per-frame median',
    'comparison': 'same Release binary; DEREN_ASYNC_PRESENT=0 versus 1'}, 'runs': {}, 'pooled': {}, 'images': {}}
combined = []
def stats(values):
    return {'median_ms': float(np.median(values)), 'p95_ms': float(np.percentile(values, 95)), 'samples': len(values)}
def load(run):
    rows = list(csv.DictReader((run / 'timings.csv').open()))
    assert len(rows) == 363, (run, len(rows))
    selected = rows[60:360]
    frame = [float(row['frame_ms']) for row in selected]
    gpu = [sum(float(value) for key, value in row.items() if key not in ('sample', 'frame_ms', 'cpu_prepare_ms')) for row in selected]
    phases = re.findall(r'cpu frame phases \(avg of 60 frames\): (.*)', (run / 'debug.log').read_text())
    assert len(phases) == 6, (run, len(phases))
    begin = [float(re.search(r'begin ([\d.]+) ms', line).group(1)) for line in phases[1:]]
    present = [float(re.search(r'present\* ([\d.]+) ms', line).group(1)) for line in phases[1:]]
    return rows, frame, gpu, begin, present
for scenario in ('static', 'rotation', 'near'):
    result['pooled'][scenario] = {}
    for variant in ('baseline', 'candidate'):
        all_frame, all_gpu, all_begin, all_present = [], [], [], []
        for rep in range(1, 4):
            key = f'{scenario}-{rep}/{variant}'
            rows, frame, gpu, begin, present = load(work / key)
            result['runs'][key] = {'frame': stats(frame), 'gpu_total': stats(gpu), 'begin_window_mean_ms': begin, 'present_window_mean_ms': present}
            all_frame += frame; all_gpu += gpu; all_begin += begin; all_present += present
            combined.extend({'run': key, **row} for row in rows)
        result['pooled'][scenario][variant] = {'frame': stats(all_frame), 'gpu_total': stats(all_gpu),
            'begin_median_window_mean_ms': float(np.median(all_begin)), 'present_median_window_mean_ms': float(np.median(all_present))}
for pair in sorted(work.iterdir()):
    if not pair.is_dir(): continue
    a_path = next((pair / 'baseline').glob('screenshot_*.png'))
    b_path = next((pair / 'candidate').glob('screenshot_*.png'))
    a = np.asarray(Image.open(a_path).convert('RGBA'))
    b = np.asarray(Image.open(b_path).convert('RGBA'))
    assert a.shape == b.shape
    difference = a.astype(np.int16) - b.astype(np.int16)
    item = {'differing_pixels': int(np.any(difference != 0, axis=2).sum()), 'maximum_channel_difference': int(np.abs(difference).max())}
    if (pair / 'baseline/capture.rgba16f').exists():
        aa = (pair / 'baseline/capture.rgba16f').read_bytes()
        bb = (pair / 'candidate/capture.rgba16f').read_bytes()
        item['hdr_bytes_equal'] = aa == bb
    result['images'][pair.name] = item
    errors = {}
    for variant in ('baseline', 'candidate'):
        log = (pair / variant / 'debug.log').read_text()
        messages = re.findall(r'^\[ERROR\].*', log, re.M)
        errors[variant] = [re.sub(r'0x[0-9a-fA-F]+', '<address>', message) for message in messages]
    item['validation_error_count'] = {variant: len(messages) for variant, messages in errors.items()}
    item['validation_errors_match_baseline'] = errors['baseline'] == errors['candidate']
    assert item['validation_errors_match_baseline'], (pair.name, errors)
    if pair.name.startswith('skin'): item['validation_error_messages'] = errors['baseline']
    assert item['differing_pixels'] == 0, (pair.name, item)
    if 'hdr_bytes_equal' in item: assert item['hdr_bytes_equal'], (pair.name, item)
    if pair.name in ('near-1', 'skin-motion-40', 'skin-taa-41'):
        for variant, path in [('baseline', a_path), ('candidate', b_path)]:
            with gzip.open(out / f'{pair.name}-{variant}.png.gz', 'wb') as f: f.write(path.read_bytes())
with gzip.open(out / 'timings.csv.gz', 'wt', newline='') as f:
    writer = csv.DictWriter(f, fieldnames=list(combined[0])); writer.writeheader(); writer.writerows(combined)
logs = {str(p.relative_to(work)): p.read_text() for p in work.glob('*/*/debug.log')}
with gzip.open(out / 'logs.json.gz', 'wt') as f: json.dump(logs, f)
sources = ['source/promise/rhi/rhi.extension.cppm', 'source/backends/vulkan/core/core.cpp', 'source/backends/vulkan/core/core.api_core.cpp', 'source/backends/vulkan/core/core.declarations.cppm', 'source/backends/vulkan/core/core.constructor.cppm', 'source/backends/vulkan/core/vma/vma.cppm', 'source/engine/runtime/runtime.cpp', 'source/engine/runtime/runtime.frames.cppm',
    'source/engine/runtime/runtime.declarations.cppm', 'source/engine/runtime/runtime.constructor.cppm']
result['provenance'] = {'candidate_executable_sha256': hashlib.sha256((build / 'deren.exe').read_bytes()).hexdigest(),
    'baseline_executable_sha256': hashlib.sha256((build / 'deren.exe').read_bytes()).hexdigest(),
    'candidate_source_sha256': {p: hashlib.sha256((repo / p).read_bytes()).hexdigest() for p in sources},
    'shader_sha256': {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in (build / 'shaders').glob('*.spv')},
    'runtime_dll_sha256': {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in build.glob('deren*.dll')},
    'configuration': {'sync': 'DEREN_ASYNC_PRESENT=0', 'async': 'DEREN_ASYNC_PRESENT=1', 'actual_swapchain_images': 3, 'frames_in_flight': 2},
    'validation_pairs': ['skin-motion-1','skin-motion-40','skin-motion-41','skin-taa-1','skin-taa-40','skin-taa-41']}
(out / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
for name in ('measure-async-present.ps1','analyze-async-present.py'):
    (out / name).write_bytes((build / name).read_bytes())
(out / 'SHA256SUMS').write_text(''.join(f'{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.name}\n' for p in sorted(out.iterdir()) if p.name != 'SHA256SUMS'))
print(json.dumps(result['pooled'], indent=2))
print('All 15 image pairs match exactly; HDR matches wherever emitted.')
