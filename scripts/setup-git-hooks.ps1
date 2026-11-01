param(
    [switch]$Uninstall
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
Push-Location $repo
try {
    if ($Uninstall) {
        git config --unset core.hooksPath
        Write-Host "Removed core.hooksPath; repository hooks are no longer active." -ForegroundColor Yellow
        return
    }

    if (-not (Test-Path ".githooks/pre-commit") -or -not (Test-Path ".githooks/pre-push")) {
        throw "missing .githooks/pre-commit or .githooks/pre-push"
    }

    git config core.hooksPath .githooks
    if ($LASTEXITCODE -ne 0) { throw "git config failed" }

    # Hooks are shell scripts. On POSIX they must be executable; Git for Windows
    # runs them through its bundled sh regardless of the Windows ACL.
    git update-index --chmod=+x .githooks/pre-commit .githooks/pre-merge-commit .githooks/pre-push 2>$null

    Write-Host "Git hooks enabled: core.hooksPath=.githooks" -ForegroundColor Green
    Write-Host "  pre-commit: requires the scripts/commit.ps1 checklist marker (wip/* branches are exempt)"
    Write-Host "  pre-merge-commit: rejects direct local merges"
    Write-Host "  pre-push: rejects master/main and requires the local-gate marker (wip/* refs are exempt)"
    Write-Host ""
    Write-Host "Release channel: scripts/commit.ps1 and scripts/push.ps1; direct git commit/push will fail."
    Write-Host "WIP channel: wip/* branches only; publish with 'python scripts/push_wip.py' (no local gate, no CI, never merged)."
}
finally {
    Pop-Location
}
