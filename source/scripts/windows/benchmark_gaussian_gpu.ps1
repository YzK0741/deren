param([string]$BuildDir = 'build-release-dyn-clang64', [string]$WorkDir = '')
$ErrorActionPreference = 'Stop'
$buildPath = (Resolve-Path -LiteralPath $BuildDir).Path
if (-not $WorkDir) { $WorkDir = Join-Path $buildPath 'gaussian-gpu-benchmark' }
$workPath = (New-Item -ItemType Directory -Force -Path $WorkDir).FullName
foreach ($scenario in @('static', 'rotation')) {
    $sweep = if ($scenario -eq 'rotation') { 0.5 } else { 0.0 }
    foreach ($repeat in 1..3) {
        Write-Output "Benchmark: $scenario repetition $repeat of 3"
        & (Join-Path $PSScriptRoot 'check_gaussian_gpu.ps1') -BuildDir $buildPath -WorkDir (Join-Path $workPath "$scenario-$repeat") -Frames 364 -Sweep $sweep -Validation $false -Timings $true
    }
}
$analysisPath = Join-Path $workPath 'summarize.py'
Set-Content -LiteralPath $analysisPath -Value @'
import csv, json, sys
from pathlib import Path
import numpy as np
root = Path(sys.argv[1])
result = {'warmup_frames': 60, 'sample_frames': 300, 'repetitions': 3, 'scenarios': {}}
for scenario in ('static', 'rotation'):
    modes = {}
    for mode in ('cpu', 'gpu'):
        runs = []
        for repeat in range(1, 4):
            with (root / f'{scenario}-{repeat}' / mode / 'timings.csv').open() as f:
                rows = list(csv.DictReader(f))
            if len(rows) < 360: raise SystemExit(f'insufficient samples: {scenario} {mode} {repeat}: {len(rows)}')
            rows = rows[60:360]
            metrics = {}
            for field in ('frame_ms', 'cpu_prepare_ms', 'gaussian prepare', 'gaussian sort', 'gaussian finalize', 'gaussian raster'):
                values = np.array([float(row[field]) for row in rows])
                if not np.isfinite(values).all(): raise SystemExit('nonfinite timing')
                metrics[field] = {'median': float(np.median(values)), 'p95': float(np.percentile(values, 95))}
            runs.append(metrics)
        modes[mode] = runs
    modes['median_frame_speedup_per_repetition'] = [modes['cpu'][i]['frame_ms']['median'] / modes['gpu'][i]['frame_ms']['median'] for i in range(3)]
    if any(speedup <= 1.0 for speedup in modes['median_frame_speedup_per_repetition']): raise SystemExit('GPU has no repeatable frame-rate improvement')
    result['scenarios'][scenario] = modes
with (root / 'performance.json').open('w') as f: json.dump(result, f, indent=2)
for scenario, modes in result['scenarios'].items():
    print(scenario, 'median frame speedup', modes['median_frame_speedup_per_repetition'])
'@
& python $analysisPath $workPath
if ($LASTEXITCODE -ne 0) { throw 'Performance acceptance failed' }
