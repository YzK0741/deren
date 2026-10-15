param([string]$Variant = 'baseline', [string]$Only = '')
$ErrorActionPreference = 'Stop'
$repo = 'C:/Users/23530/Desktop/yzk/deren'
$root = Join-Path $repo 'build-release-dyn-clang64/deformation-resources-check'
foreach ($variant in @($Variant)) {
    $runtime = if ($variant -like 'baseline*') { Join-Path $repo 'build-release-dyn-clang64/deformation-resources-check/runtime.before' } else { Join-Path $repo 'build-release-dyn-clang64' }
    foreach ($scenario in @('deferred','shadow_single','transparent_blend','sponza','clustered_lights','clustered_lights_off','laevatain_goo_toon','taa_on','scaled_taa','taa_fxaa','rt_smoke','ml_lights','ml_scaled','deformation','deformation_rt')) {
        if ($Only -and $scenario -ne $Only) { continue }
        $frames = 40
        $run = (New-Item -ItemType Directory -Force -Path (Join-Path $root "$variant-$scenario")).FullName
        $inputScenario = if ($scenario -eq 'deformation_rt') { 'deformation' } elseif ($scenario -like 'clustered_lights*' -or $scenario -like 'ml_*' -or $scenario -in @('taa_on','scaled_taa','taa_fxaa')) { 'deferred' } else { $scenario }
        $text = Get-Content -LiteralPath (Join-Path $repo ("build-release-dyn-clang64/render-check/$inputScenario.toml")) -Raw
        if ($scenario -like 'clustered_lights*' -or $scenario -like 'ml_*') {
            $enabled = if ($scenario -ne 'clustered_lights_off') { 'true' } else { 'false' }
            $text = $text -replace '(?m)^\[render\]$', "[render]`nclustered_lights = $enabled`nmegalights = false"
            $text += "`ndemo_lights = 32`ndemo_light_radius = 0.2`ndemo_light_range = 1.0`n"
        }
        if ($scenario -in @('taa_on','scaled_taa','taa_fxaa')) {
            $setting = if ($scenario -eq 'scaled_taa') { "[render]`ntaa = true`nrender_scale = 0.67" } else { "[render]`ntaa = true" }
            if ($scenario -eq 'taa_fxaa') { $setting = "[render]`ntaa = true`nfxaa = true" }
            $text = $text -replace '(?m)^\[render\]$', $setting
        }
        if ($scenario -like 'ml_*') {
            $text = $text.Replace('megalights = false','megalights = true')
            $text = $text -replace '(?m)^\[render\]$', "[render]`nrt_shadows = true"
        }
        if ($scenario -eq 'ml_scaled') {
            $text = $text -replace '(?m)^\[render\]$', "[render]`nrender_scale = 0.67`ntaa = true"
        }
        if ($scenario -eq 'deformation_rt') {
            $text = $text.Replace('gbuffer_debug = true', 'gbuffer_debug = false').Replace('taa = false','taa = true')
            $text = $text -replace '(?m)^\[render\]$', "[render]`nrt_shadows = true`nrt_skin_bake = true"
        }
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
        if ($scenario -in @('deformation','deformation_rt')) {
            foreach ($arg in @('--capture-animation-sweep','0.025')) { $start.ArgumentList.Add($arg) }
        }
        $p = [System.Diagnostics.Process]::Start($start)
        $stdout=$p.StandardOutput.ReadToEndAsync(); $stderr=$p.StandardError.ReadToEndAsync()
        if (-not $p.WaitForExit(60000)) { $p.Kill($true); throw 'orbit timeout' }
        Set-Content -LiteralPath (Join-Path $run 'stdout.log') -Value $stdout.GetAwaiter().GetResult()
        Set-Content -LiteralPath (Join-Path $run 'stderr.log') -Value $stderr.GetAwaiter().GetResult()
        if ($p.ExitCode -ne 0) { throw "orbit failed: $($p.ExitCode)" }
        $log = Get-Content -LiteralPath (Join-Path $run 'debug.log') -Raw
        if ($log -match 'VUID-|SYNC-HAZARD|Validation Error|\[ERROR\]') { throw "validation failed: $run" }
        if ($log -notmatch 'app_config: loaded startup settings') { throw "Startup settings not loaded: $run" }
        Write-Output "PASS $variant scenario=$scenario frames=$frames clean sync validation"
    }
}








