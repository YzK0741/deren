param(
    [switch]$Quiet
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
Push-Location $repo
try {
    $markerPath = Join-Path $repo "tmp/ci-pass.json"
    if (-not (Test-Path $markerPath)) {
        throw "no CI marker at tmp/ci-pass.json; run scripts/commit.ps1 or scripts/run-local-ci.ps1 -Mode Full"
    }

    $marker = Get-Content $markerPath -Raw | ConvertFrom-Json
    $head = (git rev-parse HEAD).Trim()
    $tree = (git rev-parse "HEAD^{tree}").Trim()
    $commitTime = [DateTimeOffset]::Parse((git show -s --format=%cI HEAD).Trim())
    $dirty = git status --porcelain --untracked-files=no

    if ($marker.commit -ne $head) {
        throw "CI marker is for $($marker.commit), current HEAD is $head; re-run the gate after committing"
    }
    if ($marker.tree -ne $tree) {
        throw "CI marker tree does not match HEAD's tree; the commit changed after the gate"
    }
    if ($marker.mode -ne "Full") {
        throw "CI marker mode is '$($marker.mode)', not Full"
    }
    if (-not $marker.working_tree_clean) {
        throw "CI marker was written on a dirty worktree"
    }
    if ($dirty) {
        throw "tracked files are modified after the CI marker; commit or stash before pushing"
    }

    $generated = [DateTimeOffset]::Parse($marker.generated_at)
    if ($generated -le $commitTime) {
        throw "CI marker was generated at $($marker.generated_at), not after commit time $($commitTime)"
    }

    if (-not $Quiet) {
        Write-Host "CI marker OK: commit $head, tree $tree, generated $($marker.generated_at)" -ForegroundColor Green
    }
}
finally {
    Pop-Location
}
