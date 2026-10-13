$ErrorActionPreference = 'Stop'
$repo = 'C:/Users/23530/Desktop/yzk/deren'
$root = Join-Path $repo 'build-release-dyn-clang64/offscreen-check'
$exe = Join-Path $repo 'build-release-dyn-clang64/async-present-check/runtime/deren.exe'
foreach ($mode in @('cpu','gpu')) {
    $run = (New-Item -ItemType Directory -Force -Path (Join-Path $root "reference-$mode")).FullName
    $text = Get-Content -LiteralPath (Join-Path $root "$mode/viewer.toml") -Raw
    $output = $run.Replace('\','/')
    $text = $text -replace '(?m)^screenshot_dir = .*$', "screenshot_dir = `"$output`""
    $cfg = Join-Path $run 'viewer.toml'
    Set-Content -LiteralPath $cfg -Value $text
    $start = [System.Diagnostics.ProcessStartInfo]::new($exe)
    $start.WorkingDirectory = $run
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Hidden
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.Environment['DEREN_GAUSSIAN_PATH'] = $mode
    $start.Environment['DEREN_ASYNC_PRESENT'] = '0'
    $start.Environment['DEREN_GAUSSIAN_HDR_CAPTURE'] = Join-Path $run 'capture.rgba16f'
    foreach ($arg in @('--config',$cfg,'--capture-frames','12','--capture-camera','0,0,4.6685,0.1190,0.3818,0.3433','--capture-sweep','0')) { $start.ArgumentList.Add($arg) }
    $p = [System.Diagnostics.Process]::Start($start)
    $stdout = $p.StandardOutput.ReadToEndAsync()
    $stderr = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit(60000)) { $p.Kill($true); throw 'reference timeout' }
    Set-Content -LiteralPath (Join-Path $run 'stdout.log') -Value $stdout.GetAwaiter().GetResult()
    Set-Content -LiteralPath (Join-Path $run 'stderr.log') -Value $stderr.GetAwaiter().GetResult()
    if ($p.ExitCode -ne 0) { throw "reference failed: $($p.ExitCode)" }
    Write-Output "PASS reference-$mode"
}
