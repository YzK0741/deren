param([string]$Variant = 'baseline')
$ErrorActionPreference = 'Stop'
$repo = 'C:/Users/23530/Desktop/yzk/deren'
$root = Join-Path $repo 'build-release-dyn-clang64/managed-resources-check'
foreach ($variant in @($Variant)) {
    $runtime = if ($variant -eq 'baseline') { Join-Path $repo 'build-release-dyn-clang64/managed-resources-check/runtime.before' } else { Join-Path $repo 'build-release-dyn-clang64' }
    foreach ($scenario in @('deferred','shadow_single','transparent_blend','sponza')) {
        $frames = 40
        $run = (New-Item -ItemType Directory -Force -Path (Join-Path $root "$variant-$scenario")).FullName
        $text = Get-Content -LiteralPath (Join-Path $repo ("build-release-dyn-clang64/render-check/$scenario.toml")) -Raw
        $text = $text -replace '(?m)^screenshot_dir = .*$', ('screenshot_dir = "' + $run.Replace('\','/') + '"')
        $text = $text -replace '(?m)^shaders_dir = .*$', ('shaders_dir = "' + (Join-Path $runtime 'shaders').Replace('\','/') + '"')
        $cfg = Join-Path $run 'viewer.toml'
        Set-Content -LiteralPath $cfg -Value $text
        $start = [System.Diagnostics.ProcessStartInfo]::new((Join-Path $runtime 'deren.exe'))
        $start.WorkingDirectory = $run
        $start.UseShellExecute = $false
        $start.CreateNoWindow = $true
        $start.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Hidden
        $start.RedirectStandardOutput = $true
        $start.RedirectStandardError = $true
        $start.Environment['VK_LAYER_VALIDATE_SYNC'] = '1'
        foreach ($arg in @('--config',$cfg,'--capture-frames',"$frames",'--capture-sweep','0.5')) { $start.ArgumentList.Add($arg) }
        $p = [System.Diagnostics.Process]::Start($start)
        $stdout=$p.StandardOutput.ReadToEndAsync(); $stderr=$p.StandardError.ReadToEndAsync()
        if (-not $p.WaitForExit(60000)) { $p.Kill($true); throw 'orbit timeout' }
        Set-Content -LiteralPath (Join-Path $run 'stdout.log') -Value $stdout.GetAwaiter().GetResult()
        Set-Content -LiteralPath (Join-Path $run 'stderr.log') -Value $stderr.GetAwaiter().GetResult()
        if ($p.ExitCode -ne 0) { throw "orbit failed: $($p.ExitCode)" }
        $log = Get-Content -LiteralPath (Join-Path $run 'debug.log') -Raw
        if ($log -match 'VUID-|SYNC-HAZARD|Validation Error|\[ERROR\]') { throw "validation failed: $run" }
        Write-Output "PASS $variant scenario=$scenario frames=$frames clean sync validation"
    }
}
