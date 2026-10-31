param(
    [string]$Branch = "master",
    # GitHub does not let a PR author approve their own PR. On a single-maintainer
    # repository a required approval therefore makes every PR unmergeable, so the
    # default is 0: this is the documented SOLO-MAINTAINER EXCEPTION (CONTRIBUTING.md
    # "GitHub-side protection"). Pass -Approvals 1 as soon as a second maintainer can
    # review; do not leave a real approval requirement silently disabled otherwise.
    [ValidateRange(0, 6)]
    [int]$Approvals = 0,
    [switch]$Apply
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot

if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
    throw "gh is required"
}

$slug = (gh repo view --json nameWithOwner -q .nameWithOwner).Trim()
if (-not $slug) { throw "could not resolve the GitHub repository" }

# Context names are the job names in .github/workflows/ci.yml. That workflow runs
# on pull_request (+ workflow_dispatch) only, so all three jobs are produced by the
# PR run that branch protection gates; there is no post-merge master build whose
# context would have to exist as well. Renaming a job there means renaming it here.
$payload = @{
    required_status_checks = @{
        strict   = $true
        contexts = @(
            "posix-stacktrace",
            "Release build + ctest + Debug gate + clang-tidy",
            "ASan + UBSan (headless tests)"
        )
    }
    enforce_admins = $true
    required_pull_request_reviews = @{
        dismiss_stale_reviews           = $true
        require_code_owner_reviews      = $false
        required_approving_review_count = $Approvals
    }
    restrictions                = $null
    # PRs are merged with a squash merge, so the history on master is linear. This
    # makes the squash (or rebase) merge the only way to land a PR and keeps the
    # validated PR tree as the commit that lands - the merge contract ci.yml states.
    required_linear_history     = $true
    allow_force_pushes          = $false
    allow_deletions             = $false
    required_conversation_resolution = $true
} | ConvertTo-Json -Depth 8

$uri = "repos/$slug/branches/$Branch/protection"
Write-Host "Branch protection payload for ${slug}:${Branch}" -ForegroundColor Cyan
Write-Host $payload

if ($Approvals -eq 0) {
    Write-Host ""
    Write-Host "SOLO-MAINTAINER EXCEPTION: required_approving_review_count is 0." -ForegroundColor Yellow
    Write-Host "Keep this only while a second maintainer cannot review. Re-run with -Approvals 1 when one can:" -ForegroundColor Yellow
    Write-Host "GitHub does not allow self-approval, so 0 here means 'PR required, review expected but not enforced by GitHub'." -ForegroundColor Yellow
}

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
