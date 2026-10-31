param(
    [ValidateSet("Fast", "Full")]
    [string]$Mode = "Full",
    [string]$BuildDir = "build-release-dyn-clang64",
    [switch]$NoMarker
)

# THIS IS A LOCAL GATE, NOT A CI PASS. It is a strict subset of
# .github/workflows/ci.yml that can run on this Windows checkout: the Release
# build, the backend-boundary ratchet, ctest, clang-format-check, and (in Full
# mode) the same clang-tidy list CI uses. It does NOT run the Debug compile gate,
# the ASan/UBSan job, or the POSIX stacktrace job. The marker it writes therefore
# attests to that subset only; the pre-push hook and scripts/push.ps1 treat it as
# a local prerequisite, and PR CI remains the merge authority (CONTRIBUTING.md).

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

    # "No untracked-file dependencies" is a repository rule, so a marker written
    # while untracked files exist would attest to a tree nobody can reproduce.
    # Generated output is already covered by .gitignore (build-*/ and tmp/), which
    # is why only these two locations are allowed here: the gate's own scratch and
    # the build tree this run uses (a custom -BuildDir may not be ignored yet).
    # Everything else - a scratch header, a hand-copied asset, a local tool - is a
    # dependency that is not in the repository and blocks the marker.
    function Get-UnexpectedUntracked([string]$BuildPath) {
        $raw = (& git ls-files --others --exclude-standard -z) -join ''
        if ($LASTEXITCODE -ne 0) { throw "git ls-files failed" }
        $untracked = @($raw -split "`0" | Where-Object { $_ })
        $buildFull = (Resolve-Path -LiteralPath $BuildPath).Path
        $buildRelative = ([System.IO.Path]::GetRelativePath($repo, $buildFull) -replace '\\', '/').TrimEnd('/') + '/'
        $allowed = @('tmp/', $buildRelative)
        $unexpected = foreach ($file in $untracked) {
            $normalized = $file -replace '\\', '/'
            $covered = $false
            foreach ($prefix in $allowed) {
                if ($normalized.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
                    $covered = $true
                    break
                }
            }
            if (-not $covered) { $normalized }
        }
        return @($unexpected)
    }

    Write-Host "Local gate ($Mode): Release build, backend-boundary ratchet, ctest, clang-format$(if ($Mode -eq 'Full') { ', clang-tidy' })" -ForegroundColor DarkGray
    Write-Host "Not covered here: Debug compile gate, ASan/UBSan, POSIX stacktrace (remote CI only)" -ForegroundColor DarkGray

    Invoke-Step "git diff --cached --check" { git diff --cached --check }

    Invoke-Step "Release build" { cmake --build $BuildDir --parallel 8 }

    # Cheap because the Release build above already produced the archives and the
    # DLL this reads; the CI job runs the same target for the same reason.
    Invoke-Step "backend boundary ratchet" { cmake --build $BuildDir --target check-backend-boundary }

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

    if (-not $NoMarker -and $Mode -ne "Full") {
        Write-Host ""
        Write-Host "Fast mode writes no marker: the marker is reserved for the Full gate." -ForegroundColor DarkGray
    }

    if (-not $NoMarker -and $Mode -eq "Full") {
        # The marker is intentionally outside the repository: CI cannot commit a
        # file after it runs without creating a second unverified commit, and a
        # committed marker would only assert itself. This file is a local
        # attestation of the gate above, consumed by the pre-push hook and
        # scripts/push.ps1.
        $unexpected = @(Get-UnexpectedUntracked $BuildDir)
        if ($unexpected.Count -gt 0) {
            throw ("refusing to write the local gate marker: {0} untracked file(s) are dependencies that are not in the repository (commit them, move them, or ignore generated output):`n{1}" -f $unexpected.Count, ($unexpected -join "`n"))
        }
        $dirty = git status --porcelain --untracked-files=no
        if ($LASTEXITCODE -ne 0) { throw "git status failed" }
        if ($dirty) {
            throw "refusing to write the local gate marker: tracked files are modified; commit or stash them first"
        }
        $commit = (git rev-parse HEAD).Trim()
        $tree = (git rev-parse "HEAD^{tree}").Trim()
        $commitTime = (git show -s --format=%cI HEAD).Trim()
        # Record the build tree repo-relative so the marker stays meaningful when the
        # checkout moves (and so the validator can re-apply the same allowlist).
        $buildId = ([System.IO.Path]::GetRelativePath($repo, (Resolve-Path -LiteralPath $BuildDir).Path) -replace '\\', '/')
        $gates = @("release-build", "backend-boundary", "ctest", "clang-format-check")
        if ($Mode -eq "Full") { $gates += "clang-tidy" }
        $marker = [ordered]@{
            commit = $commit
            tree = $tree
            commit_time = $commitTime
            generated_at = [DateTimeOffset]::Now.ToString("o")
            mode = $Mode
            build_dir = $buildId
            gates = $gates
            not_covered = @("debug-compile", "asan-ubsan", "posix-stacktrace")
            working_tree_clean = $true
            untracked_checked = $true
        }
        $markerPath = Join-Path $repo "tmp/local-gate-pass.json"
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $markerPath) | Out-Null
        $marker | ConvertTo-Json -Depth 4 | Set-Content -Path $markerPath -Encoding utf8
        Write-Host "Local gate marker written: $markerPath" -ForegroundColor DarkGray
    }

    Write-Host ""
    Write-Host "LOCAL GATE OK ($Mode) - subset of remote CI, not a CI pass" -ForegroundColor Green
    Write-Host "Remote PR CI adds the Debug compile gate, ASan/UBSan, and POSIX stacktrace." -ForegroundColor Yellow
}
finally {
    Pop-Location
}
