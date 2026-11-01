#!/usr/bin/env python3
"""Fail unless every CI job carries the WIP-channel guard.

`wip/*` branches are backup-only (CONTRIBUTING.md "The WIP channel"): nothing on
them may run CI, locally or remotely. That is enforced by one `if:` expression on
every job in both workflows, which is exactly the kind of rule that drifts when a
new job is added - a job without the guard would silently give a wip branch a CI
run. This check reads the workflow files as text (the same way
source/tests/test_docs.cpp reads ci.yml for its target-list invariant) and
requires the guard on every job under `jobs:`.

Usage: python scripts/checks/check_wip_ci_guard.py [workflow.yml ...]
Exit code 0 = every job guarded, 1 = at least one job is not (or nothing parsed).
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_WORKFLOWS = (
    ".github/workflows/ci.yml",
    ".github/workflows/subtilis-converter.yml",
)
# A job key is exactly two spaces of indent (`  build-and-test:`); `on:`'s branch
# filters are also two spaces deep, which is why parsing starts after `jobs:`.
JOB_LINE = re.compile(r"^ {2}([A-Za-z0-9_-]+):\s*$")
JOB_GUARD = re.compile(r"^ {4}if:.*wip/")


def job_guard_status(text):
    """Return (total, unguarded) job names from one workflow's text."""
    in_jobs = False
    jobs = []
    for line in text.splitlines():
        if line.startswith("jobs:"):
            in_jobs = True
            continue
        if not in_jobs:
            continue
        if line and not line.startswith((" ", "#")):
            break  # a new top-level key ends the jobs section
        match = JOB_LINE.match(line)
        if match:
            jobs.append([match.group(1), False])
            continue
        if jobs and JOB_GUARD.match(line) and "startsWith" in line:
            jobs[-1][1] = True
    return len(jobs), [name for name, guarded in jobs if not guarded]


def main(argv):
    paths = argv[1:] or [str(REPO_ROOT / workflow) for workflow in DEFAULT_WORKFLOWS]
    failed = False
    for path in paths:
        try:
            text = Path(path).read_text(encoding="utf-8")
        except OSError as error:
            print("{}: cannot read: {}".format(path, error))
            failed = True
            continue
        total, unguarded = job_guard_status(text)
        if total == 0:
            print("{}: no jobs parsed under 'jobs:' - the workflow shape changed; update this check".format(path))
            failed = True
        elif unguarded:
            print("{}: {} of {} job(s) lack the wip/* guard: {}".format(
                path, len(unguarded), total, ", ".join(unguarded)))
            failed = True
        else:
            print("{}: all {} job(s) carry the wip/* guard".format(path, total))
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
