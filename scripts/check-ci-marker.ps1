param(
    [switch]$Quiet
)

# Validates the LOCAL GATE marker (tmp/local-gate-pass.json), which attests to the
# subset of CI that scripts/run-local-ci.ps1 can run on this Windows checkout. It
# is not evidence that remote CI passed: PR CI is the merge authority
# (CONTRIBUTING.md). The checks here are deliberately strict about the local
# claims, because a marker that outlives the tree it describes is worse than none.

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
Push-Location $repo
try {
    $markerPath = Join-Path $repo "tmp/local-gate-pass.json"
    if (-not (Test-Path $markerPath)) {
        throw "no local gate marker at tmp/local-gate-pass.json; run scripts/commit.ps1 or scripts/run-local-ci.ps1 -Mode Full on a clean HEAD"
    }

    $marker = Get-Content $markerPath -Raw | ConvertFrom-Json
    $head = (git rev-parse HEAD).Trim()
    $tree = (git rev-parse "HEAD^{tree}").Trim()
    $commitTime = [DateTimeOffset]::Parse((git show -s --format=%cI HEAD).Trim())
    $dirty = git status --porcelain --untracked-files=no

    if ($marker.commit -ne $head) {
        throw "local gate marker is for $($marker.commit), current HEAD is $head; re-run the gate after committing"
    }
    if ($marker.tree -ne $tree) {
        throw "local gate marker tree does not match HEAD's tree; the commit changed after the gate"
    }
    if ($marker.mode -ne "Full") {
        throw "local gate marker mode is '$($marker.mode)', not Full"
    }
    if (-not $marker.working_tree_clean) {
        throw "local gate marker was written on a dirty worktree"
    }
    if (-not $marker.untracked_checked) {
        throw "local gate marker predates the untracked-file check; re-run scripts/run-local-ci.ps1 -Mode Full"
    }
    $recordedGates = @($marker.gates)
    foreach ($required in @("release-build", "backend-boundary", "ctest", "clang-format-check", "clang-tidy")) {
        if ($recordedGates -notcontains $required) {
            throw "local gate marker does not record the '$required' gate; re-run the gate on this commit"
        }
    }
    # THE GATE'S TRANSCRIPT: the marker is a claim, and this is the evidence that the run it claims
    # actually happened. A hand-written JSON with the right fields used to pass; it now also has to
    # match a log the gate produced, gate header by gate header. Deliberate fraud is not the threat
    # model (the same user can write both files) - an accidental or forgotten marker is.
    if (-not $marker.log_file -or -not $marker.log_sha256) {
        throw "local gate marker carries no gate log (it predates the log); re-run scripts/run-local-ci.ps1 -Mode Full on this commit"
    }
    $logPath = if ([System.IO.Path]::IsPathRooted($marker.log_file)) { $marker.log_file } else { Join-Path $repo $marker.log_file }
    if (-not (Test-Path -LiteralPath $logPath)) {
        throw ("the gate log named by the marker does not exist: {0}; re-run the gate" -f $marker.log_file)
    }
    $actualBytes = (Get-Item -LiteralPath $logPath).Length
    if ([int64]$marker.log_bytes -ne $actualBytes) {
        throw ("the gate log is {0} byte(s) but the marker recorded {1}; the log changed after the gate wrote it, so re-run the gate" -f $actualBytes, $marker.log_bytes)
    }
    $actualHash = (Get-FileHash -LiteralPath $logPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne $marker.log_sha256) {
        throw "the gate log's SHA-256 does not match the marker; the log changed after the gate wrote it, so re-run the gate"
    }
    $logText = Get-Content -LiteralPath $logPath -Raw
    foreach ($gate in $recordedGates) {
        if ($logText -notmatch [regex]::Escape("== $gate ==")) {
            throw ("the gate log does not contain the '{0}' step the marker claims; re-run the gate" -f $gate)
        }
    }

    if (-not $marker.build_dir) {
        throw "local gate marker does not name the build directory; re-run the gate on this commit"
    }
    if ($dirty) {
        throw "tracked files are modified after the local gate marker; commit or stash before pushing"
    }

    # Same rule scripts/run-local-ci.ps1 applied when it wrote the marker: an
    # untracked file that is not generated output is a dependency the repository
    # does not contain. Ignored paths (build-*/ and tmp/) do not appear here at
    # all, which is why only the gate's scratch and its build tree are allowed.
    $buildFull = if ([System.IO.Path]::IsPathRooted($marker.build_dir)) { $marker.build_dir } else { Join-Path $repo $marker.build_dir }
    $allowed = @('tmp/')
    if (Test-Path -LiteralPath $buildFull) {
        $buildRelative = ([System.IO.Path]::GetRelativePath($repo, (Resolve-Path -LiteralPath $buildFull).Path) -replace '\\', '/').TrimEnd('/') + '/'
        $allowed += $buildRelative
    }
    $raw = (& git ls-files --others --exclude-standard -z) -join ''
    if ($LASTEXITCODE -ne 0) { throw "git ls-files failed" }
    $unexpected = @(foreach ($file in @($raw -split "`0" | Where-Object { $_ })) {
        $normalized = $file -replace '\\', '/'
        $covered = $false
        foreach ($prefix in $allowed) {
            if ($normalized.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
                $covered = $true
                break
            }
        }
        if (-not $covered) { $normalized }
    })
    if ($unexpected.Count -gt 0) {
        throw ("untracked file(s) appeared after the local gate marker; they are dependencies that are not in the repository:`n{0}" -f ($unexpected -join "`n"))
    }

    $generated = [DateTimeOffset]::Parse($marker.generated_at)
    if ($generated -le $commitTime) {
        throw "local gate marker was generated at $($marker.generated_at), not after commit time $($commitTime)"
    }

    if (-not $Quiet) {
        Write-Host "Local gate marker OK: commit $head, tree $tree, generated $($marker.generated_at)" -ForegroundColor Green
        Write-Host "Scope: local subset of CI (no Debug compile gate, ASan/UBSan, or POSIX stacktrace); remote PR CI is the merge authority." -ForegroundColor DarkGray
    }
}
finally {
    Pop-Location
}
