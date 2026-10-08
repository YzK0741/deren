# End-to-end RT acceptance: the MASK bake produces non-indexed triangles, a path
# the small indexed acceleration-structure probe does not reach.
param(
    [string]$BuildDir = 'build-release-dyn-clang64',
    [string]$Model = 'C:\Users\23530\Desktop\yzk\glTF-Sample-Assets\Models\Sponza\glTF\Sponza.gltf',
    [int]$Frames = 40,
    [string]$Camera = '90,0,6.41,0,-18.548,0'
)
$ErrorActionPreference = 'Stop'
$build = (Resolve-Path -LiteralPath $BuildDir).Path
$work = Join-Path $build 'rt-check'
New-Item -ItemType Directory -Path $work -Force | Out-Null
Get-ChildItem -LiteralPath $work -Filter 'screenshot_*.png' | Remove-Item -Force
$config = Join-Path $work 'rt.toml'
# CPU dispatch logs can be green while raygen returns before TraceRay. Require exact GPU results first.
$probe = Join-Path $build 'test_acceleration_structures.exe'
if (-not (Test-Path -LiteralPath $probe)) { throw 'Build test_acceleration_structures before RT acceptance' }
$probeStart = [Diagnostics.ProcessStartInfo]::new()
$probeStart.FileName = $probe
$probeStart.WorkingDirectory = $work
$probeStart.UseShellExecute = $false
$probeStart.CreateNoWindow = $true
$probeStart.RedirectStandardOutput = $true
$probeStart.RedirectStandardError = $true
$probeStart.ArgumentList.Add('--with-device')
$probeProcess = [Diagnostics.Process]::Start($probeStart)
$probeOut = $probeProcess.StandardOutput.ReadToEndAsync()
$probeErr = $probeProcess.StandardError.ReadToEndAsync()
if (-not $probeProcess.WaitForExit(60000)) { $probeProcess.Kill(); throw 'GPU traversal probe timed out' }
$probeText = $probeOut.GetAwaiter().GetResult() + $probeErr.GetAwaiter().GetResult()
$probeText | Set-Content -LiteralPath (Join-Path $work 'traversal.log') -Encoding utf8
if ($probeProcess.ExitCode -ne 0 -or $probeText -notmatch 'rt_traversal: GPU results 1,0,0 / 1,0,0 .*mapped heap') {
    throw 'GPU traversal probe did not verify both TLAS slots, hits, misses and mask discard; see traversal.log'
}
$probeLog = Join-Path $work 'debug.log'
if (Test-Path -LiteralPath $probeLog) {
    Copy-Item -LiteralPath $probeLog -Destination (Join-Path $work 'traversal-debug.log') -Force
    if (Select-String -LiteralPath $probeLog -Pattern 'VUID-|Validation Error|\[ERROR\]|\[WARNING\]|panic') {
        throw 'GPU traversal probe has validation/log findings; see traversal-debug.log'
    }
}
@"
model = '$Model'
[paths]
screenshot_dir = '$work'
[render]
window_width = 1080
window_height = 960
vsync = false
validation_layers = true
taa = false
rt_shadows = true
rt_mask_bake = true
rt_skin_bake = true
[gui]
show = false
[lighting]
env_size = 256
env_mip_count = 5
irr_size = 32
lut_size = 256
"@ | Set-Content -LiteralPath $config -Encoding utf8
$start = [Diagnostics.ProcessStartInfo]::new()
$start.FileName = Join-Path $build 'deren.exe'
$start.WorkingDirectory = $work
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
foreach ($argument in @('--config', $config, '--capture-frames', "$Frames", "--capture-camera=$Camera")) {
    $start.ArgumentList.Add($argument)
}
$process = [Diagnostics.Process]::Start($start)
if (-not $process.WaitForExit(180000)) { $process.Kill(); throw 'RT smoke timed out' }
if ($process.ExitCode -ne 0) { throw "RT smoke exited $($process.ExitCode)" }
$log = Join-Path $work 'debug.log'
$bad = @(Select-String -LiteralPath $log -Pattern 'VUID-|Validation Error|\[ERROR\]|\[WARNING\]|panic|frame submit was refused')
if ($bad.Count -gt 0) {
    $bad | Select-Object -First 2 | ForEach-Object { Write-Host $_.Line }
    throw "RT smoke has $($bad.Count) validation/log findings"
}
$traced = Select-String -LiteralPath $log -Pattern 'ray-traced shadows: tracing .* rays per frame'
$baked = Select-String -LiteralPath $log -Pattern 'ray-traced shadows: [1-9][0-9]* MASK casters baked'
if (-not $traced -or -not $baked) { throw 'RT smoke did not exercise both tracing and MASK bake' }
$shots = @(Get-ChildItem -LiteralPath $work -Filter 'screenshot_*.png')
if ($shots.Count -ne 1) { throw 'RT smoke must produce exactly one screenshot' }
$baked | ForEach-Object { Write-Host $_.Line }
$traced | ForEach-Object { Write-Host $_.Line }
Write-Host "RT smoke: PASS (GPU hit/miss/mask and both TLAS slots verified; $Frames frames, clean validation, screenshot produced)"
