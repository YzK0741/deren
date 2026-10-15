# Capture identical flower scenes with the CPU and GPU paths, requiring real GPU execution and clean validation.
param(
    [string]$BuildDir = 'build-release-dyn-clang64',
    [string]$WorkDir = '',
    [int]$Frames = 12,
    [double]$Sweep = 0,
    [string]$Camera = '0,0,4.6685,0.1190,0.3818,0.3433',
    [bool]$Validation = $true,
    [bool]$Timings = $false
)
$ErrorActionPreference = 'Stop'
$buildPath = (Resolve-Path -LiteralPath $BuildDir).Path
$modelPath = 'C:/Users/23530/Desktop/yzk/deren/build-release-dyn-clang64/gaussian-viewer/flowers/flowers_1/flowers_1.ply'
if (-not (Test-Path -LiteralPath $modelPath)) { throw 'Fetch the existing flower acceptance asset first' }
if (-not $WorkDir) { $WorkDir = Join-Path $buildPath 'gaussian-gpu-check' }
$workPath = (New-Item -ItemType Directory -Force -Path $WorkDir).FullName
$captures = @{}
foreach ($mode in @('cpu', 'gpu')) {
    $runPath = (New-Item -ItemType Directory -Force -Path (Join-Path $workPath $mode)).FullName
    $configPath = Join-Path $runPath 'viewer.toml'
    $modelSetting = $modelPath.Replace('\', '/')
    $shaderSetting = (Join-Path $buildPath 'shaders').Replace('\', '/')
    $outputSetting = $runPath.Replace('\', '/')
    $validationSetting = $Validation.ToString().ToLowerInvariant()
    $timingSetting = $Timings.ToString().ToLowerInvariant()
    Set-Content -LiteralPath $configPath -Value @"
model = "$modelSetting"
grid_side = 0
[paths]
shaders_dir = "$shaderSetting"
screenshot_dir = "$outputSetting"
[render]
window_width = 1280
window_height = 720
vsync = false
max_fps = 0
taa = false
shadow = true
validation_layers = $validationSetting
gpu_timings = $timingSetting
[gui]
show = false
[lighting]
env_size = 256
env_mip_count = 5
irr_size = 32
lut_size = 256
"@
    $start = [System.Diagnostics.ProcessStartInfo]::new((Join-Path $buildPath 'deren.exe'))
    $start.WorkingDirectory = $runPath
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Hidden
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    [void]$start.Environment.Remove('VK_LAYER_ENABLES')
    $start.Environment['VK_LAYER_VALIDATE_SYNC'] = '1'
    $start.Environment['DEREN_GAUSSIAN_PATH'] = $mode
    $start.Environment['DEREN_GAUSSIAN_HDR_CAPTURE'] = (Join-Path $runPath 'capture.rgba16f')
    if ($Timings) { $start.Environment['DEREN_GAUSSIAN_BENCHMARK'] = (Join-Path $runPath 'timings.csv') }
    foreach ($argument in @('--config', $configPath, '--capture-frames', "$Frames", '--capture-camera', $Camera, '--capture-sweep', "$Sweep")) { $start.ArgumentList.Add($argument) }
    $startedAt = Get-Date
    $process = [System.Diagnostics.Process]::Start($start)
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    try {
        if (-not $process.WaitForExit(600000)) { throw "Timed out in $mode capture" }
    } finally {
        if (-not $process.HasExited) { $process.Kill($true) }
        $process.WaitForExit()
        Set-Content -LiteralPath (Join-Path $runPath 'stdout.log') -Value $stdout.GetAwaiter().GetResult()
        Set-Content -LiteralPath (Join-Path $runPath 'stderr.log') -Value $stderr.GetAwaiter().GetResult()
    }
    if ($process.ExitCode -ne 0) { throw "$mode renderer exited with $($process.ExitCode)" }
    $logPath = Join-Path $runPath 'debug.log'
    if ((Get-Item -LiteralPath $logPath).LastWriteTime -lt $startedAt) { throw "No new $mode log" }
    $log = Get-Content -LiteralPath $logPath -Raw
    if ($Validation -and $log -notmatch 'validation layers enabled') { throw 'Validation was not enabled' }
    if ($log -match 'VUID-|Validation Error|SYNC-HAZARD|\[ERROR\]|was refused|frame open is fatal|initialization: FAIL|numeric errors|HDR capture: FAIL|gaussian recording: FAIL') { throw "Rendering failed; inspect $logPath" }
    $expectedMode = $mode.ToUpperInvariant()
    if ($log -notmatch "gaussian path: $expectedMode") { throw "Requested $expectedMode did not execute" }
    $capture = Get-ChildItem -LiteralPath $runPath -Filter 'screenshot_*.png' -File |
        Where-Object { $_.LastWriteTime -ge $startedAt } | Sort-Object LastWriteTime | Select-Object -Last 1
    if (-not $capture) { throw "No new $mode screenshot" }
    $captures[$mode] = $capture.FullName
    Write-Output "PASS: $expectedMode rendered $Frames frames"
}
$comparisonPath = Join-Path $workPath 'compare.py'
Set-Content -LiteralPath $comparisonPath -Value @'
import json, sys
import numpy as np
from PIL import Image
a = np.asarray(Image.open(sys.argv[1]).convert('RGB'), dtype=np.float64)
b = np.asarray(Image.open(sys.argv[2]).convert('RGB'), dtype=np.float64)
if a.shape != b.shape: raise SystemExit('extent mismatch')
delta = a - b
rmse = np.sqrt(np.mean(delta * delta, axis=(0, 1)))
result = {'channel_rmse_255': rmse.tolist(), 'maximum_channel_difference': float(np.max(np.abs(delta))), 'differing_pixels': int(np.count_nonzero(np.any(delta != 0, axis=2)))}
with open(sys.argv[3], 'w') as output: json.dump(result, output, indent=2)
print(json.dumps(result))
if np.any(rmse > 1.0): raise SystemExit('final RGB RMSE exceeds 1/255')
from pathlib import Path
def hdr(path):
    data = Path(path).read_bytes()
    width, height = np.frombuffer(data[:8], dtype='<u4')
    return np.frombuffer(data[8:], dtype='<f2').astype(np.float64).reshape(int(height), int(width), 4)[..., :3]
ha = hdr(str(Path(sys.argv[1]).parent / 'capture.rgba16f'))
hb = hdr(str(Path(sys.argv[2]).parent / 'capture.rgba16f'))
hd = ha - hb
if not np.isfinite(hd).all(): raise SystemExit('nonfinite HDR pixels')
result['hdr_rmse'] = float(np.sqrt(np.mean(hd * hd)))
result['hdr_maximum_rgb_difference'] = float(np.max(np.abs(hd)))
with open(sys.argv[3], 'w') as output: json.dump(result, output, indent=2)
print(json.dumps(result))
if result['hdr_rmse'] > .005: raise SystemExit('HDR RMSE exceeds 0.005')
'@
& python $comparisonPath $captures.cpu $captures.gpu (Join-Path $workPath 'image-comparison.json')
if ($LASTEXITCODE -ne 0) { throw 'CPU/GPU image comparison failed' }
