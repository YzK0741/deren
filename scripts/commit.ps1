param(
    [Parameter(Mandatory = $true)]
    [string]$Message,
    [string[]]$Paths,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
Push-Location $repo
try {
    if ($Paths -and $Paths.Count -gt 0) {
        git add -- @Paths
        if ($LASTEXITCODE -ne 0) { throw "git add failed" }
    }

    $staged = git diff --cached --name-only
    if ($LASTEXITCODE -ne 0) { throw "git diff --cached failed" }
    if (-not $staged) {
        throw "nothing staged; pass -Paths or stage the intended files first"
    }

    Write-Host ""
    Write-Host "Staged files:" -ForegroundColor Cyan
    git diff --cached --stat

    Write-Host ""
    Write-Host "Before committing, answer all three questions with 'yes'." -ForegroundColor Yellow

    function Ask-Required([string]$Question) {
        $answer = Read-Host "$Question [yes/no]"
        if ($answer -notin @("yes", "y")) {
            throw "commit refused: '$Question' was not answered yes"
        }
    }

    Ask-Required "feature 是否完整 (is the feature complete)"
    Ask-Required "ci 是否本地通过 (has the local CI gate passed)"
    Ask-Required "是否确保诚实 (is this commit honest, with no hidden WIP or untracked dependency)"

    Write-Host ""
    Write-Host "Running the local CI gate (this is the same gate the commit claims passed)..." -ForegroundColor Cyan
    & pwsh -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "run-local-ci.ps1") -Mode Full
    if ($LASTEXITCODE -ne 0) {
        throw "commit refused: local CI gate failed"
    }

    if ($DryRun) {
        Write-Host ""
        Write-Host "DRY RUN: local gate passed; no commit created." -ForegroundColor Yellow
        return
    }

    $env:DEREN_CHECKLIST_OK = "1"
    try {
        git commit -m $Message
        if ($LASTEXITCODE -ne 0) { throw "git commit failed" }
    }
    finally {
        Remove-Item Env:DEREN_CHECKLIST_OK -ErrorAction SilentlyContinue
    }

    Write-Host ""
    Write-Host "Committed. Push only through scripts/push.ps1 on a feature branch." -ForegroundColor Green
}
finally {
    Pop-Location
}
