#!/usr/bin/env python3
"""Run the deren renderer from a built configuration.

Usage:
    python scripts/build_helpers/run.py run source/scripting/test_game --capture-frames 60
    python scripts/build_helpers/run.py --type Debug run build/source/scripting/test_game
    python scripts/build_helpers/run.py --build-dir build-local run <project>
    python scripts/build_helpers/run.py --dry-run run <project> --capture-frames 1

Runs from the repository root.  Everything after the wrapper's own options is
forwarded to the executable verbatim, which is how the ``run`` subcommand gets
through: the application is launched as ``deren run <project>``, and the project
directory (not a model path) is the first argument.  There is no --model,
--grid or --config: models come from the project's scene TOML, and the
application reads the project's ``deren.toml``.  The legacy -Type/-BuildDir
spellings are still accepted for callers migrating from the old platform
wrappers.
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

CONFIGS = ("Debug", "RelWithDebInfo", "Release")


def _normalize_legacy_args(argv: Sequence[str]) -> List[str]:
    values = list(argv)
    normalized: List[str] = []
    index = 0
    mapping = {
        "-Type": "--type",
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
    parser.add_argument("--type", "-t", choices=CONFIGS, default="RelWithDebInfo", help="build configuration")
    parser.add_argument("--build-dir", help="explicit build directory")
    parser.add_argument("--dry-run", action="store_true", help="print the command without running it")
    known, unknown = parser.parse_known_args(left)
    return known, explicit_forward + unknown


def main(argv: Sequence[str]) -> int:
    args, forward = _parse_args(argv)
    executable = executable_for(args.type, args.build_dir)
    command: List[str] = [str(executable)]
    command += forward
    if args.dry_run:
        print_command(command, REPO_ROOT)
        return 0
    if not executable.exists():
        print(
            "not built: {}\nrun: python scripts/build_helpers/build.py --type {}".format(
                executable, args.type
            ),
            file=sys.stderr,
        )
        return 1
    return run_command(command, cwd=REPO_ROOT, check=False)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
