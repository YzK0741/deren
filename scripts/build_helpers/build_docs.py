#!/usr/bin/env python3
"""Build the documentation: Doxygen HTML plus the LaTeX manual PDF.

Usage:
    python scripts/build_helpers/build_docs.py
    python scripts/build_helpers/build_docs.py --dry-run

The generated output stays in docs/html and docs/latex.

Behavior preserved from the old scripts:
- clear generated docs/html and docs/latex/*.pdf before Doxygen (derived state);
- find Doxygen on PATH or in standard install directories;
- find MiKTeX / TeX Live in standard locations;
- use the generated docs/latex/Makefile when make exists, otherwise a small
  pdflatex/makeindex loop with the same "Rerun to get" / "may have changed"
  detection and stale-aux recovery;
- retry the Doxygen+LaTeX sequence once for the measured xpdf/epstopdf image
  flake.
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys
from pathlib import Path
from typing import List, Optional, Tuple

from common import (
    IS_WINDOWS,
    REPO_ROOT,
    prepare_tex_environment,
    print_log_tail,
    run_command,
)

DOCS = REPO_ROOT / "docs"
LATEX = DOCS / "latex"


def find_doxygen() -> Optional[str]:
    found = shutil.which("doxygen")
    if found:
        return found
    candidates = [
        Path(r"C:\Program Files\doxygen\bin\doxygen.exe"),
        Path("/usr/bin/doxygen"),
        Path("/usr/local/bin/doxygen"),
        Path("/opt/homebrew/bin/doxygen"),
    ]
    for candidate in candidates:
        if candidate.is_file():
            return str(candidate)
    return None


def clear_generated(dry_run: bool) -> None:
    html = DOCS / "html"
    if html.exists():
        print("== clearing the generated HTML ({}) ==".format(html))
        if not dry_run:
            shutil.rmtree(html)
    if LATEX.is_dir():
        print("== clearing the doxygen-generated PDF cache ({}) ==".format(LATEX / "*.pdf"))
        if not dry_run:
            for pdf in LATEX.glob("*.pdf"):
                pdf.unlink()


def run_doxygen(doxygen: str, dry_run: bool) -> None:
    print("== doxygen: {} ==".format(doxygen))
    run_command([doxygen, "Doxyfile"], cwd=REPO_ROOT, dry_run=dry_run)
    if not dry_run:
        print("html written to docs/html/index.html")


def remove_derived_latex() -> None:
    for name in ("refman.aux", "refman.toc", "refman.out"):
        path = LATEX / name
        if path.exists():
            path.unlink()


def _tail_log(path: Path, lines: int) -> None:
    print_log_tail(path, lines)


def _run_logged(command: List[str], log_path: Path, cwd: Path, dry_run: bool) -> int:
    return run_command(command, cwd=cwd, dry_run=dry_run, check=False, log_path=log_path)


def _pdflatex(draft: bool, dry_run: bool, retry_flake: List[bool]) -> int:
    command = ["pdflatex"]
    if draft:
        command.append("-draftmode")
    command += ["-interaction=nonstopmode", "-halt-on-error", "refman.tex"]
    log = LATEX / "latex_pass.log"
    code = _run_logged(command, log, LATEX, dry_run)
    if code == 0:
        return 0
    print("pdflatex failed (see docs/latex/refman.log for the full transcript):", file=sys.stderr)
    _tail_log(log, 30)
    if log.exists():
        text = log.read_text(encoding="utf-8", errors="replace")
        if "xpdf: reading PDF image failed" in text or "Problems running epstopdf" in text:
            retry_flake[0] = True
    return code


def _makeindex(dry_run: bool) -> None:
    if not (LATEX / "refman.idx").exists():
        return
    if shutil.which("makeindex") is None:
        return
    log = LATEX / "latex_makeindex.log"
    code = _run_logged(["makeindex", "refman.idx"], log, LATEX, dry_run)
    if code != 0:
        print("makeindex failed:", file=sys.stderr)
        _tail_log(log, 20)
        raise SystemExit(code)
    if not dry_run and log.exists():
        log.unlink()


def _aux_fingerprint() -> str:
    import hashlib

    parts = []
    for name in ("refman.aux", "refman.toc", "refman.out", "refman.idx"):
        path = LATEX / name
        if path.exists():
            parts.append(hashlib.sha256(path.read_bytes()).hexdigest())
        else:
            parts.append("-")
    return ":".join(parts)


def _rerun_wanted() -> bool:
    log = LATEX / "refman.log"
    if not log.exists():
        return False
    text = log.read_text(encoding="utf-8", errors="replace")
    return "Rerun to get" in text or "may have changed" in text


def _manual_latex(dry_run: bool) -> None:
    print("== latex via pdflatex (no make found, output suppressed) ==")
    retry_flake = [False]
    if _pdflatex(True, dry_run, retry_flake) != 0:
        if retry_flake[0]:
            raise RuntimeError("retryable image flake")
        raise SystemExit(1)
    _makeindex(dry_run)
    count = 0
    fingerprint = _aux_fingerprint()
    while not dry_run and _rerun_wanted():
        if _pdflatex(True, dry_run, retry_flake) != 0:
            if retry_flake[0]:
                raise RuntimeError("retryable image flake")
            raise SystemExit(1)
        count += 1
        state = _aux_fingerprint()
        if state == fingerprint or count >= 4:
            break
        fingerprint = state
    _makeindex(dry_run)
    if _pdflatex(False, dry_run, retry_flake) != 0:
        if retry_flake[0]:
            raise RuntimeError("retryable image flake")
        raise SystemExit(1)
    if not dry_run:
        pass_log = LATEX / "latex_pass.log"
        if pass_log.exists():
            pass_log.unlink()


def _make_latex(dry_run: bool) -> None:
    make = shutil.which("make") or shutil.which("gmake")
    if make:
        print("== latex via make (docs/latex/Makefile, output suppressed) ==")
        log = LATEX / "latex_make.log"
        code = _run_logged([make], log, LATEX, dry_run)
        if code != 0:
            print("make failed (exit {}), tail of docs/latex/latex_make.log:".format(code), file=sys.stderr)
            _tail_log(log, 40)
            raise SystemExit(code)
        if not dry_run and log.exists():
            log.unlink()
        return
    if shutil.which("pdflatex"):
        _manual_latex(dry_run)
        return
    raise SystemExit("no LaTeX toolchain found. Install MiKTeX/TeX Live (or make), then rerun.")


def _compile_latex_once(dry_run: bool) -> None:
    remove_derived_latex()
    _make_latex(dry_run)


def build(dry_run: bool) -> None:
    doxygen = find_doxygen()
    if not doxygen:
        raise SystemExit("doxygen not found on PATH (install Doxygen or add its bin dir).")
    clear_generated(dry_run)
    run_doxygen(doxygen, dry_run)
    if dry_run:
        return
    if not LATEX.is_dir():
        raise SystemExit("docs/latex was not generated; check the Doxygen run.")
    prepare_tex_environment()
    try:
        _compile_latex_once(False)
    except RuntimeError:
        # The measured xpdf/epstopdf flake: Doxygen rewrites every graph image,
        # so regenerate them and retry the manual sequence once.
        print(
            "== pdflatex could not read a generated image PDF; regenerating the graphs and retrying ONCE =="
        )
        for pdf in LATEX.glob("*.pdf"):
            pdf.unlink()
        run_doxygen(doxygen, False)
        remove_derived_latex()
        try:
            _make_latex(False)
        except RuntimeError:
            raise SystemExit(
                "pdflatex failed twice reading generated image PDFs; this is not the known "
                "ephemeral flake - see docs/latex/refman.log."
            )
    if not (LATEX / "refman.pdf").exists():
        raise SystemExit("refman.pdf was not produced (see docs/latex/refman.log).")
    print("")
    print("documentation built:")
    print("  html: docs/html/index.html")
    print("  pdf:  docs/latex/refman.pdf")


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(description="Build Doxygen HTML and the LaTeX manual PDF.")
    parser.add_argument("--dry-run", action="store_true", help="print commands without running them")
    args = parser.parse_args(argv)
    build(args.dry_run)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
