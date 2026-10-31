param(
    [ValidateSet("Fast", "Full")]
    [string]$Mode = "Full",
    [string]$BuildDir = "build-release-dyn-clang64",
    [switch]$NoMarker
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
Push-Location $repo
try {
    if (-not (Test-Path (Join-Path $repo $BuildDir))) {
        throw "build directory not found: $BuildDir (configure/build first)"
    }

    function Invoke-Step([string]$Name, [scriptblock]$Body) {
        Write-Host ""
        Write-Host "== $Name ==" -ForegroundColor Cyan
        & $Body
        if ($LASTEXITCODE -ne 0) {
            throw "$Name failed (exit $LASTEXITCODE)"
        }
    }

    Invoke-Step "git diff --cached --check" { git diff --cached --check }

    Invoke-Step "Release build" { cmake --build $BuildDir --parallel 8 }

    # CTest's radiance test writes through TEMP; the system TEMP is not always
    # writable in this workspace, so pin it inside the repository.
    $tempDir = Join-Path $repo "tmp/ci-temp"
    New-Item -ItemType Directory -Force -Path $tempDir | Out-Null
    $oldTemp = $env:TEMP
    $oldTmp = $env:TMP
    $env:TEMP = $tempDir
    $env:TMP = $tempDir
    try {
        Invoke-Step "CTest" { ctest --test-dir $BuildDir --output-on-failure }
    }
    finally {
        $env:TEMP = $oldTemp
        $env:TMP = $oldTmp
    }

    Invoke-Step "clang-format-check" { cmake --build $BuildDir --target clang-format-check }

    if ($Mode -eq "Full") {
        # Parse the same list the CI job uses instead of maintaining a second one.
        $ci = Get-Content (Join-Path $repo ".github/workflows/ci.yml") -Raw
        $match = [regex]::Match($ci, 'TIDY_FILES="([\s\S]*?)"')
        if (-not $match.Success) {
            throw "could not find TIDY_FILES in .github/workflows/ci.yml"
        }
        $tidyFiles = ($match.Groups[1].Value -replace '\\\r?\n', ' ').Trim() -split '\s+'
        if ($tidyFiles.Count -eq 0) {
            throw "TIDY_FILES list is empty"
        }
        Write-Host ""
        Write-Host "== clang-tidy ($($tidyFiles.Count) files) ==" -ForegroundColor Cyan
        & python scripts/tools/run_clang_tidy.py --build-dir $BuildDir @tidyFiles
        if ($LASTEXITCODE -ne 0) {
            throw "clang-tidy failed (exit $LASTEXITCODE)"
        }
    }

    if (-not $NoMarker) {
        # The marker is intentionally outside the repository: CI cannot commit a
        # file after it runs without creating a second unverified commit, and a
        # committed marker would only assert itself. This file is a local
        # attestation consumed by the pre-push hook and scripts/push.ps1.
        $dirty = git status --porcelain --untracked-files=no
        if ($LASTEXITCODE -ne 0) { throw "git status failed" }
        if ($dirty) {
            throw "refusing to write a CI marker: tracked files are modified; commit or stash them first"
        }
        $commit = (git rev-parse HEAD).Trim()
        $tree = (git rev-parse "HEAD^{tree}").Trim()
        $commitTime = (git show -s --format=%cI HEAD).Trim()
        $marker = [ordered]@{
            commit = $commit
            tree = $tree
            commit_time = $commitTime
            generated_at = [DateTimeOffset]::Now.ToString("o")
            mode = $Mode
            build_dir = $BuildDir
            working_tree_clean = $true
        }
        $markerPath = Join-Path $repo "tmp/ci-pass.json"
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $markerPath) | Out-Null
        $marker | ConvertTo-Json -Depth 4 | Set-Content -Path $markerPath -Encoding utf8
        Write-Host "CI marker written: $markerPath" -ForegroundColor DarkGray
    }

    Write-Host ""
    Write-Host "LOCAL CI OK ($Mode)" -ForegroundColor Green
}
finally {
    Pop-Location
}
