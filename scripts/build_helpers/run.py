#!/usr/bin/env python3
"""Run the deren renderer from a built configuration.

Usage:
    python scripts/build_helpers/run.py run build/source/scripting/test_game --capture-frames 60
    python scripts/build_helpers/run.py --type Debug run build-debug/source/scripting/test_game
    python scripts/build_helpers/run.py --build-dir build-local run <project>
    python scripts/build_helpers/run.py --dry-run run <project> --capture-frames 1

Runs from the repository root.  The wrapper forwards ``run`` and every argument
after it verbatim.  Use a leading ``--`` after the wrapper options to forward a
standalone command such as ``--version``.  There is no --model,
--grid or --config: models come from the project's scene TOML, and the
application reads the project's ``deren.toml``.  The legacy -Type/-BuildDir
spellings are still accepted for callers migrating from the old platform
wrappers.
"""

from __future__ import annotations

import argparse
import sys
from typing import List, Sequence

from common import (
    REPO_ROOT,
    executable_for,
    print_command,
    run_command,
)

CONFIGS = ("Debug", "RelWithDebInfo", "Release")


def _parse_args(argv: Sequence[str]) -> (argparse.Namespace, List[str]):
    parser = argparse.ArgumentParser(
        description="Run the deren renderer.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        allow_abbrev=False,
    )
    parser.add_argument("--type", "-t", "-Type", choices=CONFIGS, default="RelWithDebInfo", help="build configuration")
    parser.add_argument("--build-dir", "-BuildDir", help="explicit build directory")
    parser.add_argument("--dry-run", action="store_true", help="print the command without running it")
    parser.add_argument("forward", nargs=argparse.REMAINDER, help="renderer command and its arguments")
    options, leading = parser.parse_known_args(argv)
    forward = options.forward
    if forward[:1] == ["--"]:
        forward = forward[1:]
    return options, leading + forward


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
