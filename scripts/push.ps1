param(
    [Parameter(Mandatory = $true)]
    [string]$Branch,
    [switch]$CreatePr,
    [string]$Title,
    [string]$Body = ""
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
Push-Location $repo
try {
    if ($Branch -in @("master", "main")) {
        throw "refusing to push to $Branch; use a feature branch and a pull request"
    }

    $current = (git branch --show-current).Trim()
    if ($current -ne $Branch) {
        throw "current branch is '$current', not '$Branch'; check it out before pushing"
    }

    if (-not $Title) { $Title = $Branch }

    Write-Host "Running the full local CI gate before push..." -ForegroundColor Cyan
    & pwsh -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "run-local-ci.ps1") -Mode Full
    if ($LASTEXITCODE -ne 0) {
        throw "push refused: local CI gate failed"
    }

    $env:DEREN_LOCAL_CI_OK = "1"
    try {
        git push -u origin $Branch
        if ($LASTEXITCODE -ne 0) { throw "git push failed" }
    }
    finally {
        Remove-Item Env:DEREN_LOCAL_CI_OK -ErrorAction SilentlyContinue
    }

    if ($CreatePr) {
        if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
            throw "gh is not installed; push succeeded, but create the PR manually"
        }
        $arguments = @("pr", "create", "--base", "master", "--head", $Branch, "--title", $Title)
        if ($Body) { $arguments += @("--body", $Body) }
        & gh @arguments
        if ($LASTEXITCODE -ne 0) { throw "gh pr create failed" }
    }

    Write-Host "Pushed $Branch. Merge only through a green PR." -ForegroundColor Green
}
finally {
    Pop-Location
}
