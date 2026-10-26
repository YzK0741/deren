#!/usr/bin/env python3
"""Shared helpers for the unified deren build/run/docs entry points.

The scripts in this directory are the cross-platform entry points.  They must
stay Python 3.8 compatible because the README only requires Python 3.8+ for
the repository tooling.  Platform setup/bootstrap remains in the platform
setup scripts: this module assumes the toolchain is already installed.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, Optional, Sequence


REPO_ROOT = Path(__file__).resolve().parents[2]
IS_WINDOWS = os.name == "nt"


def build_dir_for(config: str, explicit: Optional[str] = None) -> Path:
    """Return the build directory for a CMake configuration.

    The historical layout is kept: Windows uses build-<config>-clang64 and
    POSIX uses build-<config>.  ``explicit`` overrides either layout.
    """
    if explicit:
        path = Path(explicit)
        return path if path.is_absolute() else REPO_ROOT / path
    suffix = "-clang64" if IS_WINDOWS else ""
    return REPO_ROOT / ("build-" + config.lower() + suffix)


def executable_for(config: str, explicit_dir: Optional[str] = None) -> Path:
    directory = build_dir_for(config, explicit_dir)
    return directory / ("deren.exe" if IS_WINDOWS else "deren")


def print_command(command: Sequence[str], cwd: Optional[Path] = None) -> None:
    location = " (in {})".format(cwd) if cwd else ""
    print("+ " + " ".join(str(part) for part in command) + location)


def run_command(
    command: Sequence[str],
    *,
    cwd: Optional[Path] = None,
    env: Optional[Dict[str, str]] = None,
    dry_run: bool = False,
    check: bool = True,
    log_path: Optional[Path] = None,
) -> int:
    """Run a command, optionally capturing it to ``log_path``.

    A dry run prints the command and returns 0.  For a captured command the
    output goes to the log and the caller prints a tail on failure, matching
    the previous shell/PowerShell documentation scripts.
    """
    print_command(command, cwd)
    if dry_run:
        return 0
    if log_path is None:
        completed = subprocess.run(
            list(command),
            cwd=str(cwd) if cwd else None,
            env=env,
            check=False,
        )
    else:
        log_path.parent.mkdir(parents=True, exist_ok=True)
        with log_path.open("wb") as log:
            completed = subprocess.run(
                list(command),
                cwd=str(cwd) if cwd else None,
                env=env,
                stdout=log,
                stderr=subprocess.STDOUT,
                check=False,
            )
    if check and completed.returncode != 0:
        raise SystemExit(completed.returncode)
    return completed.returncode


def print_log_tail(log_path: Path, lines: int = 30) -> None:
    if not log_path.exists():
        return
    try:
        text = log_path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return
    for line in text[-lines:]:
        print(line, file=sys.stderr)


def find_python_command() -> List[str]:
    """Return the interpreter command used by the legacy forwarding shims."""
    if sys.executable:
        return [sys.executable]
    return [sys.executable or "python"]


def prepare_windows_environment() -> None:
    r"""Make the MSYS2 clang64 toolchain and Vulkan SDK reachable.

    Mirrors the old PowerShell build script: add C:\msys64\clang64\bin when
    clang++ is not already on PATH, and infer VULKAN_SDK from C:\VulkanSDK.
    """
    if not IS_WINDOWS:
        return
    if shutil.which("clang++") is None:
        clang64_bin = Path(r"C:\msys64\clang64\bin")
        if (clang64_bin / "clang++.exe").exists():
            os.environ["PATH"] = str(clang64_bin) + os.pathsep + os.environ.get("PATH", "")
            print("added {} to PATH".format(clang64_bin))
    if not os.environ.get("VULKAN_SDK"):
        sdk_root = Path(r"C:\VulkanSDK")
        if sdk_root.is_dir():
            candidates = sorted(
                (entry for entry in sdk_root.iterdir() if entry.is_dir()),
                key=lambda entry: entry.name,
                reverse=True,
            )
            if candidates:
                os.environ["VULKAN_SDK"] = str(candidates[0])
                print("VULKAN_SDK={}".format(candidates[0]))


def require_tools(*names: str) -> None:
    missing = [name for name in names if shutil.which(name) is None]
    if missing:
        raise SystemExit(
            "missing required tool(s): {}. Run the platform setup script first.".format(
                ", ".join(missing)
            )
        )


def add_tex_to_path(directory: Path) -> None:
    if not directory.is_dir():
        return
    path = os.environ.get("PATH", "")
    parts = path.split(os.pathsep) if path else []
    if str(directory) not in parts:
        os.environ["PATH"] = str(directory) + os.pathsep + path


def prepare_tex_environment() -> None:
    """Find standard MiKTeX/TeX Live install directories like the old scripts."""
    if IS_WINDOWS:
        local = os.environ.get("LOCALAPPDATA")
        program_files = os.environ.get("PROGRAMFILES")
        if local:
            add_tex_to_path(Path(local) / "Programs" / "MiKTeX" / "miktex" / "bin" / "x64")
        if program_files:
            add_tex_to_path(Path(program_files) / "MiKTeX" / "miktex" / "bin" / "x64")
        return
    roots = [Path("/c/texlive"), Path("/usr/local/texlive"), Path("/opt/texlive")]
    for root in roots:
        if not root.is_dir():
            continue
        for year in root.iterdir():
            bin_root = year / "bin"
            if not bin_root.is_dir():
                continue
            for arch in bin_root.iterdir():
                add_tex_to_path(arch)
