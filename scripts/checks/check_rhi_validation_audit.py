#!/usr/bin/env python3
"""Audit the RHI concrete-identity mechanism (ABI 30 names + the contract-level validate-and-cast pair).

The review record historical development record quotes a table of "how many
concrete-name checks exist, and where the remaining raw casts are". That table is easy to let drift by hand, so
this script derives it from the CURRENT tree instead. It is an audit instrument, not a gate: the numbers are
expected to move when the backend changes, and its output is what gets pasted into the record.

Run from the repository root:

    python scripts/checks/check_rhi_validation_audit.py

It answers, in order:
  1. the concrete implementation names (the single source the producers and the checks share),
  2. concrete-name string literals that leaked outside that source (should be none in the backend),
  3. `rhi::is_concrete` / `rhi::do_concrete` use: statement lines and call occurrences.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
BACKEND = ROOT / "source" / "backends" / "vulkan"
CORE = BACKEND / "core"

CONSTANT = re.compile(r'inline constexpr std::string_view (\w+) = "(deren_[a-z_]+_vulkan)";')
LITERAL = re.compile(r'"(deren_[a-z_]+_vulkan)"')
IS_CONCRETE = re.compile(r"rhi::is_concrete\(")
DO_CONCRETE = re.compile(r"rhi::do_concrete<")


def code_lines(path: Path):
    """(line number, text) for the lines that are not pure comments."""
    for number, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
        stripped = line.lstrip()
        if stripped.startswith("//") or stripped.startswith("*"):
            continue
        yield number, line


def sources():
    return sorted({*CORE.glob("*.cppm"), *CORE.glob("*.cpp"), *BACKEND.glob("*.cppm"), *BACKEND.glob("*.cpp")})


def main() -> int:
    declarations = (CORE / "core.declarations.cppm").read_text(encoding="utf-8", errors="replace")
    names = CONSTANT.findall(declarations)
    print(f"concrete name constants: {len(names)}")
    for identifier, value in names:
        print(f"  {identifier:28s} {value}")

    leaked = []
    for path in sources():
        if path.name == "core.declarations.cppm":
            continue
        for number, line in code_lines(path):
            if LITERAL.search(line):
                leaked.append(f"{path.relative_to(ROOT)}:{number}: {line.strip()}")
    print(f"\nconcrete literals outside the name table: {len(leaked)}")
    for entry in leaked:
        print(f"  {entry}")

    is_statements = is_calls = do_calls = 0
    per_file = Counter()
    for path in sources():
        for _, line in code_lines(path):
            if IS_CONCRETE.search(line):
                is_statements += 1
                is_calls += len(IS_CONCRETE.findall(line))
                per_file[str(path.relative_to(ROOT))] += 1
            do_calls += len(DO_CONCRETE.findall(line))
    print(f"\nrhi::is_concrete: {is_statements} statement lines, {is_calls} call occurrences")
    for name, count in sorted(per_file.items()):
        print(f"  {count:2d}  {name}")
    print(f"rhi::do_concrete: {do_calls} call sites")
    return 0


if __name__ == "__main__":
    sys.exit(main())
