# Build the project documentation. Forwarding shim: the unified entry point is
# scripts/unified/build_docs.py, which also works on POSIX.
#
# Usage:
#     powershell -ExecutionPolicy Bypass -File scripts/windows/build_docs.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$script = Join-Path $root 'scripts\unified\build_docs.py'
$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) {
    $python = Get-Command python3 -ErrorAction SilentlyContinue
}
if (-not $python) {
    Write-Error 'Python 3.8+ was not found. Install Python or run scripts/unified/build_docs.py with an explicit interpreter.'
    exit 1
}
& $python.Source $script @args
exit $LASTEXITCODE
