param(
    [ValidateSet("Fast", "Full")]
    [string]$Mode = "Full",
    # Empty = "the Release tree scripts/build_helpers/build.py produced" (resolved below);
    # pass a path to override.
    [string]$BuildDir = "",
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
    # WHICH BUILD TREE. The old default was a hand-made directory name (`build-release-dyn-clang64`)
    # that no documented command creates, so a fresh clone that had just run
    # `python scripts/build_helpers/build.py --type Release` got "build directory not found".
    # Default to what that builder produces - build-<config>-clang64 on Windows, build-<config> on
    # POSIX (scripts/build_helpers/common.py) - and then to the hand-made tree this checkout uses.
    # -BuildDir always wins, and a path that does not exist is still reported by name.
    $releaseCandidates = @("build-release-clang64", "build-release", "build-release-dyn-clang64")
    if (-not $BuildDir) {
        $BuildDir = @($releaseCandidates | Where-Object { Test-Path -LiteralPath (Join-Path $repo $_) })[0]
    }
    if (-not $BuildDir) {
        throw ("no Release build tree found (looked for: {0}). Configure and build one first:`n    python scripts/build_helpers/build.py --type Release`nor pass -BuildDir <path>." -f ($releaseCandidates -join ", "))
    }
    if (-not (Test-Path -LiteralPath (Join-Path $repo $BuildDir))) {
        throw ("build directory not found: {0} (run python scripts/build_helpers/build.py --type Release, or pass -BuildDir <path>)" -f $BuildDir)
    }

    # THE GATE'S TRANSCRIPT. Every step's output is tee'd into tmp/local-gate.log, and the marker
    # records that file's SHA-256: a hand-written marker with the right JSON fields used to pass
    # check-ci-marker.ps1, and now it also needs the log this run actually produced. This is
    # tamper-EVIDENT, not tamper-proof (the same user can write both files) - which is why the
    # marker still says "local prerequisite" and PR CI remains the merge authority.
    $gateLog = Join-Path $repo "tmp/local-gate.log"
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $gateLog) | Out-Null
    Set-Content -LiteralPath $gateLog -Value "" -Encoding utf8

    # THE FIRST ARGUMENT IS THE MARKER'S GATE ID, not the display name: check-ci-marker.ps1 requires
    # every gate id it reads from tmp/local-gate-pass.json to appear as a `== id ==` header in this log,
    # so the two spellings cannot drift apart. The title is only for the console.
    function Invoke-Step([string]$Id, [string]$Title, [scriptblock]$Body) {
        Write-Host ""
        Write-Host "== $Title ==" -ForegroundColor Cyan
        Add-Content -LiteralPath $gateLog -Value "" -Encoding utf8
        Add-Content -LiteralPath $gateLog -Value "== $Id ==" -Encoding utf8
        & $Body 2>&1 | Tee-Object -FilePath $gateLog -Append
        if ($LASTEXITCODE -ne 0) {
            throw "$Title failed (exit $LASTEXITCODE); the step output is in tmp/local-gate.log ($Id)"
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

    Invoke-Step "git-diff-check" "git diff --cached --check" { git diff --cached --check }

    Invoke-Step "release-build" "Release build" { cmake --build $BuildDir --parallel 8 }

    # Cheap because the Release build above already produced the archives and the
    # DLL this reads; the CI job runs the same target for the same reason.
    Invoke-Step "backend-boundary" "backend boundary ratchet" { cmake --build $BuildDir --target check-backend-boundary }

    # Some tests write through TEMP; the system TEMP is not always writable in
    # this workspace, so pin it inside the repository.
    $tempDir = Join-Path $repo "tmp/ci-temp"
    New-Item -ItemType Directory -Force -Path $tempDir | Out-Null
    $oldTemp = $env:TEMP
    $oldTmp = $env:TMP
    $env:TEMP = $tempDir
    $env:TMP = $tempDir
    try {
        Invoke-Step "ctest" "CTest" { ctest --test-dir $BuildDir --output-on-failure }
    }
    finally {
        $env:TEMP = $oldTemp
        $env:TMP = $oldTmp
    }

    Invoke-Step "clang-format-check" "clang-format-check" { cmake --build $BuildDir --target clang-format-check }

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
        Invoke-Step "clang-tidy" "clang-tidy ($($tidyFiles.Count) files)" {
            & python scripts/tools/run_clang_tidy.py --build-dir $BuildDir @tidyFiles
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
        # The transcript is the evidence behind the claim: the marker names the file and its hash, and
        # check-ci-marker.ps1 re-hashes it and requires each gate's header to be in it.
        $logHash = (Get-FileHash -LiteralPath $gateLog -Algorithm SHA256).Hash.ToLowerInvariant()
        $logBytes = (Get-Item -LiteralPath $gateLog).Length
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
            log_file = "tmp/local-gate.log"
            log_sha256 = $logHash
            log_bytes = $logBytes
        }
        $markerPath = Join-Path $repo "tmp/local-gate-pass.json"
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $markerPath) | Out-Null
        $marker | ConvertTo-Json -Depth 4 | Set-Content -Path $markerPath -Encoding utf8
        Write-Host "Local gate marker written: $markerPath" -ForegroundColor DarkGray
        Write-Host "Evidence log: $gateLog (sha256 $logHash)" -ForegroundColor DarkGray
    }

    Write-Host ""
    Write-Host "LOCAL GATE OK ($Mode) - subset of remote CI, not a CI pass" -ForegroundColor Green
    Write-Host "Remote PR CI adds the Debug compile gate, ASan/UBSan, and POSIX stacktrace." -ForegroundColor Yellow
}
finally {
    Pop-Location
}
