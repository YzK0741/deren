# Manual GPU regression: render with zero mesh primitives and zero Gaussian particles.
# The generated empty PLY avoids the application's interactive model picker.
param(
    [string]$BuildDir = 'build-release-dyn-clang64',
    [string]$WorkDir = '',
    [ValidateRange(1, 4)][int]$ShadowCascades = 3
)
$ErrorActionPreference = 'Stop'
$buildPath = (Resolve-Path -LiteralPath $BuildDir).Path
$executable = Join-Path $buildPath 'deren.exe'
if (-not (Test-Path -LiteralPath $executable)) { throw "No renderer at $executable" }
if (-not $WorkDir) { $WorkDir = Join-Path $buildPath 'empty-scene-check' }
$workPath = (New-Item -ItemType Directory -Force -Path $WorkDir).FullName
$modelPath = Join-Path $workPath 'empty.ply'
$header = @('ply', 'format ascii 1.0', 'element vertex 0')
foreach ($property in @('x', 'y', 'z', 'scale_0', 'scale_1', 'scale_2', 'rot_0', 'rot_1', 'rot_2', 'rot_3', 'opacity', 'f_dc_0', 'f_dc_1', 'f_dc_2')) {
    $header += "property float $property"
}
$header += 'end_header'
Set-Content -LiteralPath $modelPath -Value $header -Encoding ascii
$configPath = Join-Path $workPath 'empty.toml'
$modelSetting = $modelPath.Replace('\', '/')
$shaderSetting = (Join-Path $buildPath 'shaders').Replace('\', '/')
$outputSetting = $workPath.Replace('\', '/')
Set-Content -LiteralPath $configPath -Value @"
model = "$modelSetting"
grid_side = 0
[paths]
shaders_dir = "$shaderSetting"
screenshot_dir = "$outputSetting"
[render]
window_width = 640
window_height = 360
vsync = false
taa = false
shadow = true
shadow_cascades = $ShadowCascades
validation_layers = true
[gui]
show = false
[lighting]
env_size = 256
env_mip_count = 5
irr_size = 32
lut_size = 256
"@
$start = [System.Diagnostics.ProcessStartInfo]::new($executable)
$start.WorkingDirectory = $workPath
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Hidden
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
[void]$start.Environment.Remove('VK_LAYER_ENABLES')
$start.Environment['VK_LAYER_VALIDATE_SYNC'] = '1'
foreach ($argument in @('--config', $configPath, '--capture-frames', '8', '--capture-camera', '0,0,5,0,0,0')) { $start.ArgumentList.Add($argument) }
$startedAt = Get-Date
$process = [System.Diagnostics.Process]::Start($start)
$standardOutput = $process.StandardOutput.ReadToEndAsync()
$standardError = $process.StandardError.ReadToEndAsync()
try {
    if (-not $process.WaitForExit(55000)) { throw 'Empty-scene check timed out' }
} finally {
    if (-not $process.HasExited) { $process.Kill($true) }
    $process.WaitForExit()
    Set-Content -LiteralPath (Join-Path $workPath 'stdout.log') -Value $standardOutput.GetAwaiter().GetResult()
    Set-Content -LiteralPath (Join-Path $workPath 'stderr.log') -Value $standardError.GetAwaiter().GetResult()
}
if ($process.ExitCode -ne 0) { throw "Renderer exited with $($process.ExitCode)" }
$logPath = Join-Path $workPath 'deren-debug.log'
if ((Get-Item -LiteralPath $logPath).LastWriteTime -lt $startedAt) { throw 'No new renderer log' }
$log = Get-Content -LiteralPath $logPath -Raw
if ($log -notmatch 'validation layers enabled') { throw 'Validation was not enabled' }
if ($log -match 'VUID-|Validation Error|SYNC-HAZARD|\[ERROR\]|was refused|frame open is fatal|(?:Gaussian.*probe|GPU sort probe): FAIL') { throw "Rendering failed; inspect $logPath" }
if ($log -notmatch 'runtime scene tree: 0 roots, 0 nodes \(0 leaf primitives\)') { throw 'The input scene is not empty' }
if ($log -notmatch 'gaussian: read 0 splats') { throw 'The input cloud is not empty' }
if ($log -notmatch 'shadow mapping enabled:') { throw 'Startup shadow configuration was skipped before the first frame' }
$screenshot = Get-ChildItem -LiteralPath $workPath -Filter 'screenshot_*.png' -File |
    Where-Object { $_.LastWriteTime -ge $startedAt } | Sort-Object LastWriteTime | Select-Object -Last 1
if (-not $screenshot) { throw 'No new screenshot: a zero exit code alone does not prove a frame rendered' }
Write-Output "PASS: empty scene rendered with $ShadowCascades shadow cascades and clean synchronization validation"
Write-Output $screenshot.FullName
