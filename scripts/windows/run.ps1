# Run deren (Windows). Forwarding shim: the unified entry point is
# scripts/unified/run.py, which also works on POSIX.
#
# Usage:
#     powershell -ExecutionPolicy Bypass -File scripts/windows/run.ps1
#     powershell -ExecutionPolicy Bypass -File scripts/windows/run.ps1 -Model path/to/model.glb
#     powershell -ExecutionPolicy Bypass -File scripts/windows/run.ps1 -Type Debug -- --version
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$script = Join-Path $root 'scripts\unified\run.py'
$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) {
    $python = Get-Command python3 -ErrorAction SilentlyContinue
}
if (-not $python) {
    Write-Error 'Python 3.8+ was not found. Install Python or run scripts/unified/run.py with an explicit interpreter.'
    exit 1
}
& $python.Source $script @args
exit $LASTEXITCODE
