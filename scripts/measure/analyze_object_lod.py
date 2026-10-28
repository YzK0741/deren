"""Validate complete object-LOD capture matrices and recompute their summaries.

Missing/failed runs or missing metrics are errors, never substituted with zero.
"""
import argparse
import copy
import json
import math
from pathlib import Path
import statistics
import re
import hashlib
import struct


NUMBER = r'([0-9.eE+-]+)'

def parse_log(text):
    metrics = {'validation_clean': not re.search(r'VUID-|Validation Error|\[ERROR\]|\[WARNING\]|panic|recorded out of order', text)}
    gpu = re.findall(r'gpu pass timings \(session avg of (\d+) frames\):([^\n]+)', text)
    if gpu:
        total = re.search(r'\btotal '+NUMBER+r' ms', gpu[-1][1])
        if total:
            metrics['gpu_ms'] = float(total[1])
            metrics['gpu_frames'] = int(gpu[-1][0])
    cpu = re.findall(r'cpu frame phases \(avg of \d+ frames\):([^\n]+)', text)
    windows = re.findall(r'gpu pass timings \(avg of (\d+) frames over '+NUMBER+r' s\)', text)
    if windows:
        metrics['frame_wall_ms'] = 1000 * float(windows[-1][1]) / int(windows[-1][0])
    if cpu:
        total = re.search(r'\btotal '+NUMBER+r' ms', cpu[-1])
        pace = re.search(r'\bpace '+NUMBER+r' ms', cpu[-1])
        if total and pace:
            metrics['cpu_active_ms'] = float(total[1]) - float(pace[1])
            metrics['cpu_frame_ms'] = float(total[1])
            metrics['cpu_pace_ms'] = float(pace[1])
    cut = re.findall(r'subtilis CPU cut: frames (\d+), work '+NUMBER+r' us/frame, wall '+NUMBER+r' us/frame, '+NUMBER+r' us/item', text)
    if cut:
        metrics.update(zip(('cut_frames', 'cut_work_us', 'cut_wall_us', 'cut_us_per_item'), map(float, cut[-1]), strict=True))
    publication = re.findall(r'subtilis: cut run publication: (\d+) -> (\d+) records, (\d+) -> (\d+) triangles, (\d+) fallback item', text)
    if publication:
        metrics.update(zip(('input_records', 'selected_records', 'input_triangles', 'selected_triangles', 'cut_fallbacks'), map(int, publication[-1]), strict=True))
    capacity = re.findall(r'subtilis capacity: items (\d+) peak (\d+), table (\d+)/(\d+), max run (\d+), refused frames (\d+)', text)
    if capacity:
        metrics.update(zip(('visible_items', 'peak_items', 'table_records', 'table_capacity', 'max_run', 'capacity_frames'), map(int, capacity[-1]), strict=True))
    imported = re.findall(r'subtilis import: (\d+) adopted \((\d+) shared\), (\d+) LOD0 fallback', text)
    if imported:
        metrics.update(zip(('adopted', 'import_cache_hits', 'import_fallbacks'), map(int, imported[-1]), strict=True))
    startup = re.findall(r'model loaded \+ environment cubemap \(startup window incl. runtime init\): '+NUMBER+r' ms', text)
    if startup:
        metrics['model_environment_startup_ms'] = float(startup[-1])
    return metrics



def summarize(data):
    if type(data.get('rounds')) is not int or data['rounds'] < 3:
        raise ValueError('at least three complete rounds are required')
    runs = data['runs']
    expected = {(camera, validation, on, r) for camera in ('far', 'near')
                for validation in (False, True) for on in (False, True)
                for r in range(1, data['rounds'] + 1)}
    keys = [(r['camera'], r['validation'], r['on'], r['round']) for r in runs]
    if len(set(keys)) != len(keys) or set(keys) != expected:
        raise ValueError('incomplete or duplicate camera/validation/OFF-ON/round matrix')
    result = {}
    for run in runs:
        if run.get('exit_code') != 0 or not run.get('validation_clean') or run.get('frames', 0) < 130:
            raise ValueError('unsuccessful, unvalidated or short capture')
        required = ['gpu_ms', 'frame_wall_ms', 'cpu_active_ms', 'process_wall_seconds', 'import_ready_seconds', 'peak_working_set_bytes']
        if run['on']:
            required += ['cut_work_us', 'cut_wall_us', 'cut_us_per_item', 'selected_triangles', 'selected_records', 'adopted']
        for key in required:
            v = run.get(key)
            if not isinstance(v, (int, float)) or not math.isfinite(v) or v < 0:
                raise ValueError(f'missing/invalid {key} in {run.get("tag")}')
        if run['on'] and run['adopted'] == 0:
            raise ValueError('ON capture adopted no DAGs')
    for camera in ('far', 'near'):
        for validation in (False, True):
            for on in (False, True):
                rows = [r for r in runs if (r['camera'], r['validation'], r['on']) == (camera, validation, on)]
                metrics = ['gpu_ms', 'frame_wall_ms', 'cpu_active_ms', 'process_wall_seconds', 'import_ready_seconds', 'peak_working_set_bytes']
                if on:
                    metrics += ['cut_work_us', 'cut_wall_us', 'cut_us_per_item', 'selected_triangles', 'selected_records', 'adopted']
                result[f'{camera}-validation-{validation}-on-{on}'] = {
                    key: statistics.median(r[key] for r in rows) for key in metrics}
    return result


def self_test():
    data = {'rounds': 3, 'runs': []}
    for c in ('far', 'near'):
        for v in (False, True):
            for on in (False, True):
                for r in range(1, 4):
                    row = dict(camera=c, validation=v, on=on, round=r, exit_code=0, validation_clean=True, frames=130)
                    for key in ['gpu_ms', 'frame_wall_ms', 'cpu_active_ms', 'process_wall_seconds', 'import_ready_seconds',
                                'peak_working_set_bytes', 'cut_work_us', 'cut_wall_us', 'cut_us_per_item',
                                'selected_triangles', 'selected_records', 'adopted']:
                        row[key] = r
                    data['runs'].append(row)
    assert next(iter(summarize(data).values()))['gpu_ms'] == 2
    bad_cases = []
    bad = copy.deepcopy(data); bad['runs'].pop(); bad_cases.append(bad)
    bad = copy.deepcopy(data); bad['runs'][0]['exit_code'] = 1; bad_cases.append(bad)
    bad = copy.deepcopy(data); bad['runs'][0]['gpu_ms'] = float('nan'); bad_cases.append(bad)
    bad = copy.deepcopy(data); bad['runs'][0]['frames'] = 3; bad_cases.append(bad)
    bad = copy.deepcopy(data); bad['runs'][3]['adopted'] = 0; bad_cases.append(bad)
    bad_cases.append({'rounds': 3, 'runs': []})
    for bad in bad_cases:
        try:
            summarize(bad)
        except (ValueError, KeyError):
            continue
        raise AssertionError('invalid measurement accepted')
    print('object LOD analysis gate: valid matrix + 6 negative controls passed')


def report_data(data, medians, original_image=None):
    import numpy as np
    from PIL import Image
    output = {'medians': medians, 'images': {}, 'load_medians': {}, 'capacity': {}}
    for camera in ('far', 'near'):
        images = {}
        for on in (False, True):
            rows = [r for r in data['runs'] if r['camera'] == camera and r['on'] == on]
            hashes = set()
            reference = None
            variation = []
            for row in rows:
                pixels = np.array(Image.open(row['image']).convert('RGB'))
                hashes.add(hashlib.sha256(pixels.tobytes()).hexdigest())
                if reference is None:
                    reference = pixels
                jitter = np.abs(pixels.astype(np.int16)-reference.astype(np.int16))
                variation.append({'tag': row['tag'], 'changed_pixels': int(np.any(jitter, axis=2).sum()),
                                  'mean_channel_delta': float(jitter.mean()), 'max_channel_delta': int(jitter.max())})
                if not row['validation'] and row['round'] == 1:
                    images[on] = pixels
            output['images'][f'{camera}-on{int(on)}-repeatability'] = {'pixel_sha256': sorted(hashes), 'variation': variation}
        delta = np.abs(images[False].astype(np.int16) - images[True].astype(np.int16))
        output['images'][camera] = {'changed_pixels': int(np.any(delta, axis=2).sum()),
                                    'pixels': int(delta.shape[0]*delta.shape[1]),
                                    'mean_channel_delta': float(delta.mean()), 'max_channel_delta': int(delta.max())}
        pairs = []
        for validation in (False, True):
            for number in range(1, data['rounds']+1):
                rows = [r for r in data['runs'] if (r['camera'], r['validation'], r['round']) == (camera, validation, number)]
                off = next(r for r in rows if not r['on'])
                on = next(r for r in rows if r['on'])
                a = np.array(Image.open(off['image']).convert('RGB'), dtype=np.int16)
                b = np.array(Image.open(on['image']).convert('RGB'), dtype=np.int16)
                diff = np.abs(a-b)
                pairs.append({'validation': validation, 'round': number, 'changed_pixels': int(np.any(diff, axis=2).sum()),
                              'mean_channel_delta': float(diff.mean()), 'max_channel_delta': int(diff.max())})
        output['images'][camera]['pairs'] = pairs
        if camera == 'far' and original_image:
            original = np.array(Image.open(original_image).convert('RGB'))
            if original.shape != images[False].shape or np.any(original != images[False]):
                raise ValueError('cooked OFF differs from original Bistro OFF')
            output['images']['original_off_changed_pixels'] = 0
        rows = [r for r in data['runs'] if r['camera'] == camera and r['on']]
        output['capacity'][camera] = {key: max(r[key] for r in rows) for key in
                                     ('visible_items', 'peak_items', 'table_records', 'table_capacity', 'max_run', 'capacity_frames',
                                      'cut_fallbacks', 'import_fallbacks', 'import_cache_hits', 'adopted')}
        output['capacity'][camera]['host_write_bytes'] = max(r['selected_records']*48 + r['visible_items']*112 for r in rows)
    loads = data.get('load_runs', [])
    for on in (False, True):
        rows = [r for r in loads if r['on'] == on]
        if len(rows) != 3 or any(r['exit_code'] or not r['validation_clean'] or r['frames'] != 3 for r in rows):
            raise ValueError('missing/failed 3-frame load baselines')
        output['load_medians'][str(on)] = {k: statistics.median(r[k] for r in rows) for k in
                                         ('process_wall_seconds', 'import_ready_seconds', 'peak_working_set_bytes')}
    probes = data.get('probes', [])
    if {r['camera'] for r in probes} != {'far', 'near'} or len(probes) != 2:
        raise ValueError('missing far/near probe captures')
    output['probe_checks'] = {}
    for row in probes:
        if row['exit_code'] or not row['validation_clean']:
            raise ValueError('failed probe capture')
        directory = Path(row['log']).parent
        clusters = (directory/'clusters.bin').read_bytes()
        depth = (directory/'depth.bin').read_bytes()
        width, height = struct.unpack_from('<II', clusters)
        if struct.unpack_from('<II', depth) != (width, height) or len(depth) != 8+width*height*4:
            raise ValueError('probe depth extent/size mismatch')
        z = np.frombuffer(depth, dtype='<f4', offset=8)
        if not np.isfinite(z).all() or (z < 0).any() or (z > 1).any():
            raise ValueError('invalid probe depth')
        items = struct.unpack_from('<I', clusters, 136)[0]
        cursor, records, triangles = 140, 0, 0
        for _ in range(items):
            count = struct.unpack_from('<I', clusters, cursor+64)[0]
            cursor += 68
            bounds = np.frombuffer(clusters, dtype='<f4', offset=cursor, count=count*5).reshape(count, 5)
            if not np.isfinite(bounds).all() or (bounds[:, 3] < 0).any() or (bounds[:, 4] % 3 != 0).any():
                raise ValueError('invalid probe cluster records')
            records += count
            triangles += int(bounds[:, 4].sum())//3
            cursor += count*20
        if cursor != len(clusters) or (items, records, triangles) != (row['visible_items'], row['selected_records'], row['selected_triangles']):
            raise ValueError('probe is not the actual selected run recorded by the renderer')
        output['probe_checks'][row['camera']] = {'items': items, 'records': records, 'triangles': triangles,
                                                'extent': [width, height], 'non_background_depth_pixels': int((z < 1).sum())}
    return output


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('input', type=Path, nargs='?')
    p.add_argument('--out', type=Path)
    p.add_argument('--self-test', action='store_true')
    p.add_argument('--report-data', action='store_true', help='also verify image determinism, load baselines and probes')
    p.add_argument('--original-image', type=Path)
    p.add_argument('--refresh-metrics', action='store_true', help='add newly supported metrics from raw logs; existing values must agree')
    a = p.parse_args()
    if a.self_test:
        self_test()
        return
    if not a.input:
        p.error('input is required')
    data = json.loads(a.input.read_text(encoding='utf-8'))
    for run in data['runs']:
        log = Path(run['log'])
        actual = parse_log(log.read_text(encoding='utf-8', errors='replace'))
        required = ['gpu_ms', 'frame_wall_ms', 'cpu_active_ms']
        if run['on']:
            required += ['cut_work_us', 'cut_wall_us', 'cut_us_per_item', 'selected_triangles',
                         'selected_records', 'adopted', 'visible_items', 'peak_items', 'table_records',
                         'max_run', 'cut_fallbacks', 'import_fallbacks', 'capacity_frames']
        if any(key not in actual for key in required):
            raise ValueError(f'incomplete raw log metrics: {run.get("tag")}')
        for key, value in actual.items():
            if key not in run and a.refresh_metrics:
                run[key] = value
            elif run.get(key) != value:
                raise ValueError(f'recorded metric differs from raw log: {run.get("tag")} {key}')
    result = summarize(data)
    if a.report_data:
        result = report_data(data, result, a.original_image)
    if a.refresh_metrics:
        a.input.write_text(json.dumps(data, indent=2)+'\n', encoding='utf-8')
    raw = json.dumps(result, indent=2) + '\n'
    if a.out:
        a.out.write_text(raw, encoding='utf-8')
    print(raw)


if __name__ == '__main__':
    main()
