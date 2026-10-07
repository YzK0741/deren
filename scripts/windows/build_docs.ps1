# Build the project documentation: Doxygen HTML (docs/html) + the LaTeX manual
# compiled to PDF (docs/latex/refman.pdf).
# PowerShell (Windows): run from anywhere (the script resolves the repo root from its own dir):
#     powershell -ExecutionPolicy Bypass -File scripts/windows/build_docs.ps1
# Requires: doxygen on PATH (or the standard Windows install dir) and a TeX
# toolchain — make + pdflatex/makeindex, latexmk, or bare pdflatex (MiKTeX's
# per-user install under %LOCALAPPDATA% is found automatically).
#
# The LaTeX steps run SILENTLY: the (very chatty) pdflatex/make/makeindex
# stdout+stderr goes to a throwaway log file instead of the console. On
# failure the tail of that log is printed so a broken build still says why;
# pdflatex additionally keeps its full transcript in docs/latex/refman.log.

$ErrorActionPreference = 'Stop'

# project root = two levels up from scripts/<platform>/ holding this script
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $root) {
    $root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))
}
Push-Location $root

# ---------- 1. doxygen: HTML + LaTeX sources ----------
$doxygen = Get-Command doxygen -ErrorAction SilentlyContinue
if (-not $doxygen) {
    $candidate = 'C:\Program Files\doxygen\bin\doxygen.exe'
    if (Test-Path $candidate) {
        $doxygen = Get-Item $candidate
    }
}
if (-not $doxygen) {
    Write-Error "doxygen not found on PATH (install Doxygen or add its bin dir)."
    exit 1
}

# DOXYGEN ALSO GENERATES THE GRAPHS THE MANUAL INCLUDES, and those images are the one flaky step in
# this build. MEASURED (2026-10-07, twice in the lead's runs and once here): a build stops in pdflatex
# with
#     error: Problems running epstopdf. Check your TeX installation!
#     !pdfTeX error: pdflatex.exe (file ./struct..._walker.pdf): xpdf: reading PDF image failed
#     ==> Fatal error occurred, no output PDF file produced!
# while the doxygen step itself was clean (0 warnings), and an immediate rerun of THIS SCRIPT succeeds
# with the same sources. The truncated image is written by doxygen's epstopdf pass, so a copy left by
# an earlier build can survive into the next one. TWO MEASURES, both cheap and both here:
#   1. CLEAR THE GENERATED PDF CACHE BEFORE DOXYGEN - the graph images and the previous manual are
#      derived state, and doxygen rewrites every image it needs; nothing else is touched (the `.tex`,
#      the `.sty` and doxygen's other output are regenerated in place, and refman.pdf is the product).
#   2. RETRY THE WHOLE DOXYGEN+LATEX SEQUENCE ONCE IF PDFLATEX DIES READING AN IMAGE (see the tail).
#      Only that signature is retried: every other failure still stops the build on its first exit.
# THE HTML OUTPUT IS CLEARED TOO, for the same "derived state" reason plus one measured effect: doxygen
# rewrites every page it generates, but it does NOT remove pages a previous build left behind, and an
# orphan page still links to the groups of ITS build. MEASURED right after the group rename: this tree's
# docs/html still held classvulkan_1_1runtime*.html from an older build, and those two pages alone carried
# 185 links to the retired group__vulkan__runtime.html - so the manual ON DISK could show a group that no
# longer exists anywhere in the source. Clearing the output directory makes what is on disk exactly what
# this run produced.
Write-Host '== clearing the generated HTML (docs/html) =='
if (Test-Path 'docs\html') {
    Remove-Item 'docs\html' -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host '== clearing the doxygen-generated PDF cache (docs/latex/*.pdf) =='
$stalePdfs = Join-Path 'docs\latex' '*.pdf'
if (Test-Path (Split-Path $stalePdfs)) {
    Remove-Item $stalePdfs -ErrorAction SilentlyContinue
}

function Invoke-Doxygen {
    Write-Host "== doxygen: $($doxygen.Source) =="
    & $doxygen.Source 'Doxyfile'
    if ($LASTEXITCODE -ne 0) {
        Write-Error "doxygen failed (exit $LASTEXITCODE)."
        exit $LASTEXITCODE
    }
    Write-Host 'html written to docs/html/index.html'
}

Invoke-Doxygen

# ---------- 2. LaTeX manual -> refman.pdf ----------
# put a TeX toolchain on PATH when it lives at a standard location
function Add-TexDir([string]$dir) {
    if (-not $dir -or -not (Test-Path $dir)) {
        return
    }
    if ($env:PATH -split ';' -notcontains $dir) {
        $env:PATH = "$dir;$env:PATH"
    }
}

Add-TexDir (Join-Path $env:LOCALAPPDATA 'Programs\MiKTeX\miktex\bin\x64')
Add-TexDir (Join-Path $env:ProgramFiles 'MiKTeX\miktex\bin\x64')

# Run a native build step with its stdout+stderr captured into $LogFile instead
# of the console; return the exit code. $ErrorActionPreference is relaxed around
# the call so native stderr (redirected as error records on PowerShell 5.1) does
# not trip the script-wide 'Stop'.
function Invoke-BuildStep {
    param(
        [string]$FilePath,
        [string[]]$ArgumentList,
        [string]$LogFile
    )
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $FilePath @ArgumentList *> $LogFile
        return $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previous
    }
}

# print the tail of a captured log (failure diagnostics); no-op when absent
function Show-LogTail {
    param([string]$Path, [int]$Lines = 30)
    if (Test-Path $Path) {
        Get-Content $Path -Tail $Lines | ForEach-Object { Write-Host $_ }
    }
}

# manual rerun loop replicating the generated Makefile: pdflatex, makeindex,
# repeat pdflatex while the log asks for another pass, makeindex, pdflatex
#
# WHAT COUNTS AS "THE LOG ASKS FOR ANOTHER PASS": LaTeX and hyperref ask in
# prose - "Rerun to get cross-references right", "Rerun to get outlines right",
# "Label(s) may have changed" - while the rerunfilecheck package prints
# "... Rerun checks for auxiliary files (HO)" in its identification banner on
# EVERY pass. Matching the bare word "Rerun" matched that banner, so the loop
# always ran to its cap and a converged manual was typeset ten times: ~10
# minutes, eight passes of which changed nothing. (Measured on a converged
# pass: refman.log holds three /Rerun/ lines and all three are that banner.)
function Test-RerunWanted {
    Select-String -Path 'refman.log' -Pattern 'Rerun to get|may have changed' -Quiet -ErrorAction SilentlyContinue
}

# the auxiliary state a pass is supposed to converge: identical before and after
# a pass means the pass changed nothing, whatever its log says
function Get-AuxFingerprint {
    $files = 'refman.aux', 'refman.toc', 'refman.out', 'refman.idx'
    ($files | ForEach-Object {
            if (Test-Path $_) { (Get-FileHash $_ -Algorithm SHA256).Hash } else { '-' }
        }) -join ':'
}

# $script:RetryableLatexFlake is set (instead of exiting) when pdflatex died READING ONE OF THE
# GENERATED IMAGE PDFs - the measured epstopdf/xpdf flake this script now handles (see the note above).
$script:RetryableLatexFlake = $false
function Invoke-Pdflatex {
    param([switch]$Draft)
    $cmdArgs = @('-interaction=nonstopmode', '-halt-on-error')
    if ($Draft) { $cmdArgs += '-draftmode' }
    $cmdArgs += 'refman.tex'
    $code = Invoke-BuildStep 'pdflatex' $cmdArgs 'latex_pass.log'
    if ($code -ne 0) {
        Write-Error 'pdflatex failed (see docs/latex/refman.log for the full transcript):'
        Show-LogTail 'latex_pass.log'
        $imageRead = (Select-String -Path 'latex_pass.log' -Pattern 'xpdf: reading PDF image failed|Problems running epstopdf' -Quiet -ErrorAction SilentlyContinue)
        if ($imageRead) {
            $script:RetryableLatexFlake = $true
            return
        }
        exit $code
    }
}

function Invoke-Makeindex {
    if ((Test-Path 'refman.idx') -and (Get-Command makeindex -ErrorAction SilentlyContinue)) {
        $code = Invoke-BuildStep 'makeindex' @('refman.idx') 'latex_makeindex.log'
        if ($code -ne 0) {
            Write-Error 'makeindex failed:'
            Show-LogTail 'latex_makeindex.log'
            exit $code
        }
        Remove-Item 'latex_makeindex.log' -ErrorAction SilentlyContinue
    }
}

function Invoke-LatexManual {
    # only the LAST pass writes the PDF: the earlier ones run in -draftmode,
    # which still produces the .aux/.toc/.out/.idx state they exist to converge
    Invoke-Pdflatex -Draft
    if ($script:RetryableLatexFlake) { return }
    Invoke-Makeindex
    $count = 0
    $fingerprint = Get-AuxFingerprint
    while (Test-RerunWanted) {
        Invoke-Pdflatex -Draft
        if ($script:RetryableLatexFlake) { return }
        $count++
        $state = Get-AuxFingerprint
        if (($state -eq $fingerprint) -or ($count -ge 4)) { break }
        $fingerprint = $state
    }
    Invoke-Makeindex
    Invoke-Pdflatex
    Remove-Item 'latex_pass.log' -ErrorAction SilentlyContinue
}

Set-Location 'docs\latex'

# CLEAR THE CROSS-REFERENCE STATE FIRST, and this is a measured recovery rather than tidiness: doxygen
# has just rewritten every .tex, but refman.aux survives from whatever ran before - and an aux that was
# cut short (an interrupted run leaves its last \newlabel half-written) makes pdflatex die at
# \begin{document} with "! File ended while scanning use of \@newl@bel." BEFORE it can rewrite the aux,
# so with -halt-on-error every later run fails the same way and the manual pass loop below cannot
# escape it. Measured once: refman.aux ended in "...pass_a96ba48ba179bae21943322ce7ad1e00a}{" and every
# run failed at l.201 until the file was deleted. These three are pure derived state LaTeX regenerates.
# refman.idx and refman.ind are deliberately NOT touched: the index is makeindex's input, and refman.tex
# \inputs the .ind, so removing either would fail a pass that the loop has not reached makeindex by yet.
foreach ($stale in 'refman.aux', 'refman.toc', 'refman.out') {
    Remove-Item $stale -ErrorAction SilentlyContinue
}

if (Get-Command make -ErrorAction SilentlyContinue) {
    # doxygen generates docs/latex/Makefile with 'all' -> refman.pdf
    Write-Host '== latex via make (docs/latex/Makefile, output suppressed) =='
    $make = Get-Command make -ErrorAction SilentlyContinue
    $code = Invoke-BuildStep $make.Source @() 'latex_make.log'
    if ($code -ne 0) {
        Write-Error "make failed (exit $code), tail of docs/latex/latex_make.log:"
        Show-LogTail 'latex_make.log'
        exit $code
    }
    Remove-Item 'latex_make.log' -ErrorAction SilentlyContinue
} elseif (Get-Command pdflatex -ErrorAction SilentlyContinue) {
    Write-Host '== latex via pdflatex (no make found, output suppressed) =='
    Invoke-LatexManual
    if ($script:RetryableLatexFlake) {
        # THE ONE RETRY, AND ONLY FOR THIS SIGNATURE: doxygen rewrites every graph image it needs, so
        # re-running it is what replaces a truncated one. A second failure exits normally (and loudly).
        Write-Host '== pdflatex could not read a generated image PDF; regenerating the graphs and retrying ONCE =='
        Remove-Item (Join-Path '.' '*.pdf') -ErrorAction SilentlyContinue
        Pop-Location
        Invoke-Doxygen
        Set-Location 'docs\latex'
        foreach ($stale in 'refman.aux', 'refman.toc', 'refman.out') {
            Remove-Item $stale -ErrorAction SilentlyContinue
        }
        $script:RetryableLatexFlake = $false
        Invoke-LatexManual
        if ($script:RetryableLatexFlake) {
            Write-Error 'pdflatex failed twice reading generated image PDFs; this is not the known ephemeral flake - see docs/latex/refman.log.'
            exit 1
        }
    }
} else {
    Write-Error 'no LaTeX toolchain found. Install MiKTeX/TeX Live (or make), then rerun.'
    exit 1
}

Pop-Location

if (-not (Test-Path 'docs\latex\refman.pdf')) {
    Write-Error 'refman.pdf was not produced (see docs/latex/refman.log).'
    exit 1
}
Write-Host ''
Write-Host 'documentation built:'
Write-Host '  html: docs/html/index.html'
Write-Host '  pdf:  docs/latex/refman.pdf'
exit 0
