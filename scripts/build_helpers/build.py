#!/usr/bin/env python3
"""Configure and build deren with CMake, on Windows or POSIX.

Usage:
    python scripts/build_helpers/build.py
    python scripts/build_helpers/build.py Debug
    python scripts/build_helpers/build.py --type Release --clean
    python scripts/build_helpers/build.py --dry-run Debug
    python scripts/build_helpers/build.py --build-dir build-local Release

The default builds Debug and Release in the historical directories
(build-<config>-clang64 on Windows, build-<config> elsewhere).  The legacy
-Type/-Clean spellings are still accepted for callers migrating from the old
platform wrappers.
"""

from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path
from typing import List, Optional, Sequence

from common import (
    IS_WINDOWS,
    REPO_ROOT,
    build_dir_for,
    prepare_windows_environment,
    require_tools,
    run_command,
)

CONFIGS = ("Debug", "Release")


def _normalize_legacy_args(argv: Sequence[str]) -> List[str]:
    """Map the old PowerShell spellings onto the unified CLI."""
    normalized: List[str] = []
    index = 0
    while index < len(argv):
        value = argv[index]
        if value == "-Type":
            normalized.append("--type")
            index += 1
            if index >= len(argv):
                raise SystemExit("-Type needs a value")
            normalized.append(argv[index])
        elif value in ("-Clean", "-clean"):
            normalized.append("--clean")
        elif value in ("-BuildDir", "-build-dir"):
            normalized.append("--build-dir")
            index += 1
            if index >= len(argv):
                raise SystemExit("-BuildDir needs a value")
            normalized.append(argv[index])
        elif value in ("-DryRun", "-DryRun:"):
            normalized.append("--dry-run")
        elif value == "-Type:":
            # PowerShell occasionally renders the switch as -Type:; the value
            # follows as the next argument.
            normalized.append("--type")
            index += 1
            if index >= len(argv):
                raise SystemExit("-Type: needs a value")
            normalized.append(argv[index])
        else:
            normalized.append(value)
        index += 1
    return normalized


def _parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Configure and build deren (Debug and Release by default).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Examples:\n"
            "  python scripts/build_helpers/build.py\n"
            "  python scripts/build_helpers/build.py --type Release --clean\n"
            "  python scripts/build_helpers/build.py --build-dir build-local Release\n"
        ),
    )
    parser.add_argument(
        "configs",
        nargs="*",
        metavar="CONFIG",
        help="Debug and/or Release; default: both",
    )
    parser.add_argument(
        "--type",
        action="append",
        choices=CONFIGS,
        help="configuration to build; may be repeated",
    )
    parser.add_argument("--clean", action="store_true", help="remove the build directory first")
    parser.add_argument(
        "--build-dir",
        help="explicit build directory (only valid for one configuration)",
    )
    parser.add_argument("--dry-run", action="store_true", help="print commands without running them")
    args = parser.parse_args(_normalize_legacy_args(argv))

    configs: List[str] = list(args.type or [])
    for value in args.configs:
        if value.lower() in ("clean",):
            args.clean = True
        elif value in CONFIGS:
            configs.append(value)
        else:
            parser.error("unknown configuration {!r}; expected Debug, Release or Clean".format(value))
    args.configs = configs or ["Debug", "Release"]
    if args.build_dir and len(args.configs) != 1:
        parser.error("--build-dir requires exactly one configuration")
    return args


def build(config: str, *, build_dir: Optional[str], clean: bool, dry_run: bool) -> None:
    directory = build_dir_for(config, build_dir)
    print("")
    print("== configure + build {} -> {} ==".format(config, directory))
    if clean and directory.exists():
        print("removing {}".format(directory))
        if not dry_run:
            shutil.rmtree(directory)
    if not (directory / "CMakeCache.txt").exists():
        run_command(
            ["cmake", "-S", str(REPO_ROOT), "-B", str(directory), "-G", "Ninja",
             "-DCMAKE_BUILD_TYPE={}".format(config)],
            cwd=REPO_ROOT,
            dry_run=dry_run,
        )
    run_command(["cmake", "--build", str(directory)], cwd=REPO_ROOT, dry_run=dry_run)


def main(argv: Sequence[str]) -> int:
    args = _parse_args(argv)
    prepare_windows_environment()
    require_tools("cmake", "ninja")
    for config in args.configs:
        build(config, build_dir=args.build_dir, clean=args.clean, dry_run=args.dry_run)
    print("")
    print("build complete:")
    for config in args.configs:
        executable = build_dir_for(config, args.build_dir) / ("deren.exe" if IS_WINDOWS else "deren")
        print("  {}  ({})".format(executable, config))
    if not args.dry_run:
        print("run with: python scripts/build_helpers/run.py")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
