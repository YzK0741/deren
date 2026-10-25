# Runs the opt-in numerical raster probe through a hidden two-frame application run.
param(
    [Parameter(Mandatory = $true)][string]$Config,
    [string]$BuildDir = "build-release-dyn-clang64",
    [string]$WorkDir = ""
)
$ErrorActionPreference = "Stop"
$buildPath = (Resolve-Path -LiteralPath $BuildDir).Path
$configPath = (Resolve-Path -LiteralPath $Config).Path
$executable = Join-Path $buildPath "deren.exe"
if (-not (Test-Path -LiteralPath $executable)) { throw "No renderer at $executable" }
if (-not $WorkDir) { $WorkDir = Join-Path $buildPath "gaussian-raster-probe" }
$workPath = (New-Item -ItemType Directory -Force -Path $WorkDir).FullName
$start = [System.Diagnostics.ProcessStartInfo]::new($executable)
$start.WorkingDirectory = $workPath
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Hidden
$start.Environment["DEREN_GAUSSIAN_PROBE"] = "1"
foreach ($argument in @("--config", $configPath, "--capture-frames", "2")) { $start.ArgumentList.Add($argument) }
$process = [System.Diagnostics.Process]::Start($start)
if (-not $process.WaitForExit(45000)) {
    $process.Kill($true)
    $process.WaitForExit(1000) | Out-Null
    throw "Gaussian probe timed out; inspect $workPath/deren-debug.log"
}
if ($process.ExitCode -ne 0) { throw "Renderer exited with $($process.ExitCode); inspect $workPath/deren-debug.log" }
$logPath = Join-Path $workPath "deren-debug.log"
$result = Select-String -LiteralPath $logPath -Pattern "Gaussian raster probe: (PASS|FAIL)"
if (-not $result -or $result[-1].Line -notmatch "Gaussian raster probe: PASS") { throw "Gaussian probe failed; inspect $logPath" }
if (Select-String -LiteralPath $logPath -Pattern "Validation Error|VUID-") { throw "Vulkan validation reported an error; inspect $logPath" }
$result[-1].Line
