# Optional device-probe lane for the ABI 57 lazy-resource and deferred-recording contract.
#
# CI is headless and cannot create a Vulkan device, so these probes are deliberately outside headless CTest
# (see CMakeLists.txt "Device-only ... deliberately outside headless CTest"). Run this script on a machine with
# a GPU before a release/checkpoint that touches the recorder, resource groups, submission lifetimes, the frame
# path or the Gaussian GPU path. It records check counts and refuses a target below its expected minimum
# (minimum counts are the 2026-10-25 baseline, update them when a probe legitimately grows).
param(
    [string]$BuildDir = "build-release-dyn-clang64",
    [string]$OutFile = ""
)
$ErrorActionPreference = "Stop"
$root = Resolve-Path $BuildDir
$probes = @(
    @{ name = "test_resource_group";          expected = 221 },
    @{ name = "test_resource_group_lazy"; exe = "test_resource_group"; arguments = @("--with-device", "--lazy"); expected = 320 },
    @{ name = "test_texture_group";           expected = 116 },
    @{ name = "test_submission_completion";   expected = 75 },
    @{ name = "test_render_graph_frame";      expected = 12 },
    @{ name = "test_gaussian_gpu_frame";      expected = 65 },
    @{ name = "test_buffer_transfer";         expected = 65 },
    @{ name = "test_subtilis_transfer";       expected = 56 }
)
$results = @()
$failed = 0
foreach ($probe in $probes) {
    $exeName = if ($probe.exe) { $probe.exe } else { $probe.name }
    $exe = Join-Path $root "$exeName.exe"
    if (-not (Test-Path $exe)) {
        Write-Host "[missing] $($probe.name)" -ForegroundColor Red
        $results += [pscustomobject]@{ name = $probe.name; exit = "missing"; checks = 0; expected = $probe.expected; status = "MISSING" }
        $failed++
        continue
    }
    [string[]]$probeArgs = if ($probe.arguments) { $probe.arguments } else { @("--with-device") }
    $output = & $exe @probeArgs 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $line = ($output | Select-Object -Last 1)
    $checks = 0; $bad = 0
    if ($line -match '(\d+) checks, (\d+) failed') { $checks = [int]$matches[1]; $bad = [int]$matches[2] }
    $ok = ($code -eq 0) -and ($bad -eq 0) -and ($checks -ge $probe.expected)
    if (-not $ok) { $failed++ }
    $status = if ($ok) { "PASS" } else { "FAIL" }
    Write-Host ("[{0}] {1} checks={2} expected>={3} exit={4}" -f $status, $probe.name, $checks, $probe.expected, $code) -ForegroundColor $(if ($ok) { "Green" } else { "Red" })
    $results += [pscustomobject]@{ name = $probe.name; exit = $code; checks = $checks; expected = $probe.expected; status = $status }
}
$summary = [pscustomobject]@{
    generated = (Get-Date).ToString("yyyy-MM-ddTHH:mm:ssK")
    build_dir = $root.Path
    probes = $results
    failed = $failed
}
Write-Host ""
Write-Host ("device probes: {0}/{1} passed" -f ($results.Count - $failed), $results.Count)
if ($OutFile) {
    $summary | ConvertTo-Json -Depth 4 | Set-Content -Path $OutFile -Encoding utf8
    Write-Host "summary written to $OutFile"
}
if ($failed -gt 0) { exit 1 }
