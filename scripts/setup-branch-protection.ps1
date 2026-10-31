param(
    [string]$Branch = "master",
    [switch]$Apply
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot

if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
    throw "gh is required"
}

$slug = (gh repo view --json nameWithOwner -q .nameWithOwner).Trim()
if (-not $slug) { throw "could not resolve the GitHub repository" }

# Context names are the job names in .github/workflows/ci.yml.
$payload = @{
    required_status_checks = @{
        strict   = $true
        contexts = @(
            "Release build + ctest + Debug gate + clang-tidy",
            "ASan + UBSan (headless tests)",
            "posix-stacktrace"
        )
    }
    enforce_admins = $true
    required_pull_request_reviews = @{
        dismiss_stale_reviews           = $true
        require_code_owner_reviews      = $false
        required_approving_review_count = 0
    }
    restrictions                = $null
    required_linear_history     = $false
    allow_force_pushes          = $false
    allow_deletions             = $false
    required_conversation_resolution = $true
} | ConvertTo-Json -Depth 8

$uri = "repos/$slug/branches/$Branch/protection"
Write-Host "Branch protection payload for $slug:$Branch" -ForegroundColor Cyan
Write-Host $payload

if (-not $Apply) {
    Write-Host ""
    Write-Host "Dry run only. Re-run with -Apply to PUT this protection." -ForegroundColor Yellow
    Write-Host "This requires admin permission and replaces the branch's existing protection." -ForegroundColor Yellow
    return
}

Push-Location $repoRoot
try {
    $payload | gh api --method PUT $uri --input -
    if ($LASTEXITCODE -ne 0) { throw "branch protection update failed" }
    Write-Host "Branch protection applied to $Branch." -ForegroundColor Green
}
finally {
    Pop-Location
}
