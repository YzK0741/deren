# Render the same scene with the GUI off and on. Initialization logs alone do not
# prove that the overlay drew: a private, uninitialized GLFW copy produced no panel.
param(
    [string]$BuildDir = 'build-release-dyn-clang64',
    [string]$Model = 'C:\Users\23530\Desktop\yzk\glTF-Sample-Assets\Models\DamagedHelmet\glTF\DamagedHelmet.gltf',
    [int]$Frames = 40
)
$ErrorActionPreference = 'Stop'
$build = (Resolve-Path -LiteralPath $BuildDir).Path
$exe = Join-Path $build 'deren.exe'
$work = Join-Path $build 'gui-check'
New-Item -ItemType Directory -Path $work -Force | Out-Null

function Capture-Gui([bool]$Show) {
    $name = if ($Show) { 'on' } else { 'off' }
    $run = Join-Path $work $name
    New-Item -ItemType Directory -Path $run -Force | Out-Null
    Get-ChildItem -LiteralPath $run -Filter 'screenshot_*.png' | Remove-Item -Force
    $config = Join-Path $run 'gui.toml'
    $visible = if ($Show) { 'true' } else { 'false' }
    @"
model = '$Model'
[paths]
screenshot_dir = '$run'
[render]
window_width = 640
window_height = 480
vsync = false
validation_layers = true
[gui]
show = $visible
[lighting]
env_size = 256
env_mip_count = 5
irr_size = 32
lut_size = 256
"@ | Set-Content -LiteralPath $config -Encoding utf8
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $exe
    $start.WorkingDirectory = $run
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    foreach ($argument in @('--config', $config, '--capture-frames', "$Frames", '--capture-camera=35,20,7,0,-1.6,0')) {
        $start.ArgumentList.Add($argument)
    }
    $process = [Diagnostics.Process]::Start($start)
    if (-not $process.WaitForExit(180000)) { $process.Kill(); throw "GUI $name capture timed out" }
    if ($process.ExitCode -ne 0) { throw "GUI $name capture exited $($process.ExitCode)" }
    $log = Join-Path $run 'debug.log'
    $bad = Select-String -LiteralPath $log -Pattern 'VUID-|Validation Error|\[ERROR\]|\[WARNING\]|panic'
    if ($bad) { $bad | ForEach-Object { Write-Host $_.Line }; throw "GUI $name validation/log failure" }
    if ($Show) {
        $expected = Select-String -LiteralPath $log -Pattern 'gui plugin: deren_gui_vulkan is up|gui: Dear ImGui debug overlay enabled|gui_content: ImGui overlay initialized'
        if ($expected.Count -ne 3) { throw 'Missing plugin/panel/backend initialization evidence' }
        if (Select-String -LiteralPath $log -Pattern 'display content scale 0\.00') {
            throw 'GUI platform queries returned zero scale: the plugin GLFW copy is inactive'
        }
        $expected | ForEach-Object { Write-Host $_.Line }
    }
    $shots = @(Get-ChildItem -LiteralPath $run -Filter 'screenshot_*.png')
    if ($shots.Count -ne 1) { throw "GUI $name capture must produce exactly one screenshot" }
    return $shots[0].FullName
}

$off = Capture-Gui $false
$on = Capture-Gui $true
if ((Get-FileHash -LiteralPath $off).Hash -eq (Get-FileHash -LiteralPath $on).Hash) {
    throw 'GUI on/off images are identical: the panel did not draw'
}
Write-Host "GUI smoke: PASS ($Frames frames each, clean validation, on/off images differ)"
Write-Host "GUI screenshot: $on"
