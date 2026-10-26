#!/usr/bin/env python3
"""Run the deren renderer from a built configuration.

Usage:
    python scripts/unified/run.py
    python scripts/unified/run.py --type Debug
    python scripts/unified/run.py --model path/to/model.glb --demo gui
    python scripts/unified/run.py --build-dir build-local -- --version

Runs from the repository root, matching the old PowerShell/POSIX run
scripts.  Arguments after ``--`` (and unknown options) are forwarded to the
executable.  The old -Type/-Model/-Grid/-Config/-BuildDir spellings are
accepted for the forwarding shims.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import List, Sequence

from common import (
    REPO_ROOT,
    executable_for,
    print_command,
    run_command,
)

CONFIGS = ("Debug", "Release")


def _normalize_legacy_args(argv: Sequence[str]) -> List[str]:
    values = list(argv)
    normalized: List[str] = []
    index = 0
    mapping = {
        "-Type": "--type",
        "-Config": "--config",
        "-Model": "--model",
        "-Grid": "--grid",
        "-BuildDir": "--build-dir",
    }
    while index < len(values):
        value = values[index]
        if value in mapping:
            normalized.append(mapping[value])
            index += 1
            if index >= len(values):
                raise SystemExit("{} needs a value".format(value))
            normalized.append(values[index])
        else:
            normalized.append(value)
        index += 1
    return normalized


def _split_forward(args: Sequence[str]) -> (List[str], List[str]):
    if "--" in args:
        index = args.index("--")
        return list(args[:index]), list(args[index + 1 :])
    return list(args), []


def _parse_args(argv: Sequence[str]) -> (argparse.Namespace, List[str]):
    left, explicit_forward = _split_forward(_normalize_legacy_args(argv))
    parser = argparse.ArgumentParser(
        description="Run the deren renderer.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--type", "-t", choices=CONFIGS, default="Release", help="build configuration")
    parser.add_argument("--config", help="config.toml passed as --config")
    parser.add_argument("--model", help="model path passed as the first executable argument")
    parser.add_argument("--grid", type=int, default=0, help="grid side passed after the model")
    parser.add_argument("--build-dir", help="explicit build directory")
    parser.add_argument("--dry-run", action="store_true", help="print the command without running it")
    known, unknown = parser.parse_known_args(left)
    return known, explicit_forward + unknown


def main(argv: Sequence[str]) -> int:
    args, forward = _parse_args(argv)
    executable = executable_for(args.type, args.build_dir)
    command: List[str] = [str(executable)]
    if args.config:
        command += ["--config", args.config]
    if args.model:
        command.append(args.model)
    if args.grid > 0:
        command.append(str(args.grid))
    command += forward
    if args.dry_run:
        print_command(command, REPO_ROOT)
        return 0
    if not executable.exists():
        print(
            "not built: {}\nrun: python scripts/unified/build.py --type {}".format(
                executable, args.type
            ),
            file=sys.stderr,
        )
        return 1
    return run_command(command, cwd=REPO_ROOT, check=False)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
