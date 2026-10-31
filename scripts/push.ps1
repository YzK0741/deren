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

    Write-Host "Checking the CI marker for HEAD..." -ForegroundColor Cyan
    & pwsh -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "check-ci-marker.ps1")
    if ($LASTEXITCODE -ne 0) { throw "push refused: CI marker is missing or stale" }

    $answer = Read-Host "push 完备? [y/N]"
    if ($answer -notin @("yes", "y")) {
        throw "push cancelled"
    }

    git push -u origin $Branch
    if ($LASTEXITCODE -ne 0) { throw "git push failed" }

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
