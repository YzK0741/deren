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

    Ask-Required "Is the feature complete?"
    Ask-Required "Has the local CI gate passed?"
    Ask-Required "Is this commit honest, with no hidden WIP or untracked dependency?"

    if ($DryRun) {
        Write-Host ""
        Write-Host "DRY RUN: running the gate against HEAD without creating a commit." -ForegroundColor Yellow
        & pwsh -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "run-local-ci.ps1") -Mode Full -NoMarker
        if ($LASTEXITCODE -ne 0) { throw "local CI gate failed" }
        return
    }

    $previous = (git rev-parse HEAD).Trim()
    Remove-Item (Join-Path $repo "tmp/ci-pass.json") -ErrorAction SilentlyContinue

    # Commit first, then attest it: the marker must be generated AFTER the commit
    # it names, otherwise a later amend could silently reuse a stale pass.
    $env:DEREN_CHECKLIST_OK = "1"
    try {
        git commit -m $Message
        if ($LASTEXITCODE -ne 0) { throw "git commit failed" }
    }
    finally {
        Remove-Item Env:DEREN_CHECKLIST_OK -ErrorAction SilentlyContinue
    }

    try {
        Write-Host ""
        Write-Host "Running the local CI gate against the new commit..." -ForegroundColor Cyan
        & pwsh -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "run-local-ci.ps1") -Mode Full
        if ($LASTEXITCODE -ne 0) { throw "local CI gate failed" }
    }
    catch {
        Remove-Item (Join-Path $repo "tmp/ci-pass.json") -ErrorAction SilentlyContinue
        git reset --soft $previous | Out-Null
        throw "commit rolled back: local CI failed; the staged changes are intact"
    }

    Write-Host ""
    Write-Host "Committed. Push only through scripts/push.ps1 on a feature branch." -ForegroundColor Green
}
finally {
    Pop-Location
}
