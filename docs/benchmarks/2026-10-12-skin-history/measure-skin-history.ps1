param([switch]$SkinOnly, [string]$OnlySkin = '')
$ErrorActionPreference = 'Stop'
$repo = 'C:/Users/23530/Desktop/yzk/deren'
$build = "$repo/build-release-dyn-clang64"
$work = (New-Item -ItemType Directory -Force "$build/skin-history-check").FullName
$frozen = "$build/gaussian-baseline-f57c54b2/runtime"
function Capture([string]$Scenario, [int]$Rep, [string]$Variant, [int]$Frames, [bool]$Validation) {
    $run = (New-Item -ItemType Directory -Force "$work/$Scenario-$Rep/$Variant").FullName
    $runtime = if ($Variant -eq 'baseline') { $frozen } else { $build }
    $isSkin = $Scenario.StartsWith('skin')
    $model = if ($isSkin) { "$repo/source/tests/fixtures/animated_skin_plane.gltf" } else { "$build/gaussian-viewer/flowers/flowers_1/flowers_1.ply" }
    $camera = if ($isSkin) { '0,0,4,0,0,0' } elseif ($Scenario -eq 'near') { '0,0,3.0,0.1190,0.3818,0.3433' } else { '0,0,4.6685,0.1190,0.3818,0.3433' }
    $sweep = if ($Scenario -eq 'rotation') { '0.5' } else { '0' }
    $skinStep = if ($isSkin) { '0.02' } else { '0' }
    $taa = ($Scenario -eq 'skin-taa').ToString().ToLowerInvariant()
    $debug = ($Scenario -eq 'skin-motion').ToString().ToLowerInvariant()
    $valid = $Validation.ToString().ToLowerInvariant()
    $shader = "$runtime/shaders"
    $output = $run.Replace('\','/')
    $config = "$run/viewer.toml"
    Set-Content -LiteralPath $config -Value @"
model = "$model"
grid_side = 0
[paths]
shaders_dir = "$shader"
screenshot_dir = "$output"
[render]
window_width = 1280
window_height = 720
vsync = false
max_fps = 0
taa = $taa
shadow = true
validation_layers = $valid
gpu_timings = true
gbuffer_debug = $debug
gbuffer_channel = 8
[gui]
show = false
[lighting]
env_size = 256
env_mip_count = 5
irr_size = 32
lut_size = 256
"@
    $start = [System.Diagnostics.ProcessStartInfo]::new("$runtime/deren.exe")
    $start.WorkingDirectory = $run
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Hidden
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    [void]$start.Environment.Remove('VK_LAYER_ENABLES')
    $start.Environment['VK_LAYER_VALIDATE_SYNC'] = '1'
    $start.Environment['DEREN_GAUSSIAN_PATH'] = 'gpu'
    $start.Environment['DEREN_GAUSSIAN_HDR_CAPTURE'] = "$run/capture.rgba16f"
    $start.Environment['DEREN_GAUSSIAN_BENCHMARK'] = "$run/timings.csv"
    foreach ($arg in @('--config',$config,'--capture-frames',"$Frames",'--capture-camera',$camera,'--capture-sweep',$sweep,'--capture-animation-sweep',$skinStep)) { $start.ArgumentList.Add($arg) }
    $process = [System.Diagnostics.Process]::Start($start)
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    try { if (-not $process.WaitForExit(120000)) { throw "Timed out: $Scenario $Variant" } }
    finally {
        if (-not $process.HasExited) { $process.Kill($true) }
        $process.WaitForExit()
        Set-Content "$run/stdout.log" $stdout.GetAwaiter().GetResult()
        Set-Content "$run/stderr.log" $stderr.GetAwaiter().GetResult()
    }
    if ($process.ExitCode -ne 0) { throw "Renderer exited $($process.ExitCode)" }
    $log = Get-Content "$run/debug.log" -Raw
    # Skin captures expose validation problems in the frozen baseline. Retain both logs and
    # require the analyzer to match normalized error messages and counts, as well as pixels.
    $failures = if ($isSkin) { 'was refused|initialization: FAIL|numeric errors|HDR capture: FAIL|gaussian recording: FAIL' } else { 'VUID-|Validation Error|SYNC-HAZARD|\[ERROR\]|was refused|initialization: FAIL|numeric errors|HDR capture: FAIL|gaussian recording: FAIL' }
    if ($log -match $failures) { throw "Inspect $run/debug.log" }
    if ($Validation -and $log -notmatch 'validation layers enabled') { throw 'Validation missing' }
    if (-not (Get-ChildItem $run -Filter 'screenshot_*.png')) { throw 'Screenshot missing' }
    Write-Output "$Scenario $Rep ${Variant}: completed $Frames frames"
}
if (-not $SkinOnly) { foreach ($scenario in @('static','rotation','near')) {
    foreach ($rep in 1..3) {
        $order = if ($rep -eq 2) { @('candidate','baseline') } else { @('baseline','candidate') }
        foreach ($variant in $order) { Capture $scenario $rep $variant 364 $false }
    }
} }
foreach ($scenario in @('skin-motion','skin-taa')) {
    if ($OnlySkin -and $scenario -ne $OnlySkin) { continue }
    foreach ($frames in @(1,40,41)) {
        foreach ($variant in @('baseline','candidate')) { Capture $scenario $frames $variant $frames $true }
    }
}
