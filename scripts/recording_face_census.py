#!/usr/bin/env python3
"""census the recording face by CONVENTION, not by grep: the plan's section 9.1 numbers.

WHY A SCRIPT AND NOT A GREP: the plan's first census reported `vkCmdPipelineBarrier2` as ZERO engine
call sites while 52 real ones sat in the same files it scanned, and the two sweep numbers this effort
is measured by are only comparable if the convention is fixed. The convention (plan section 9.1):

  * engine files = runtime/** + vulkan/** MINUS vulkan/core/ (that directory is the BACKEND),
    extensions .cppm / .cpp / .hpp;
  * a CALL SITE is a line that is not a comment and contains `vkCmd<Name>(` - comments are stripped,
    block comments included, because this repository's comments name these verbs constantly;
  * the second number counts engine files that really `#include <vulkan/...>` (again: code, not a
    comment quoting one).

Baseline at abi 20 before any pass migration (plan section 9.1): 140 call sites over 16 spellings,
of which 52 are `vkCmdPipelineBarrier2`; 80 engine files, 62 of them including a Vulkan header.
Target: call sites 0, including files <= 7 (the escape bucket).
"""
import collections
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BACKEND_PREFIX = "vulkan/core/"
EXTENSIONS = (".cppm", ".cpp", ".hpp")


def strip_comments(text: str) -> str:
    """Removes // to end of line and /* ... */ blocks, leaving line structure intact."""
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def engine_files():
    files = []
    for base in ("runtime", "vulkan"):
        for root, _dirs, names in os.walk(os.path.join(ROOT, base)):
            for name in names:
                if not name.endswith(EXTENSIONS):
                    continue
                rel = os.path.relpath(os.path.join(root, name), ROOT).replace("\\", "/")
                if rel.startswith(BACKEND_PREFIX):
                    continue
                files.append(rel)
    for extra in ("main.cpp", "chores.cppm"):
        if os.path.isfile(os.path.join(ROOT, extra)):
            files.append(extra)
    return sorted(files)


def main() -> int:
    call_re = re.compile(r"\bvkCmd(\w+)\s*\(")
    include_re = re.compile(r"#\s*include\s*<vulkan/")
    per_verb = collections.Counter()
    per_file = collections.Counter()
    with_vulkan_header = []
    files = engine_files()
    for rel in files:
        text = strip_comments(open(os.path.join(ROOT, rel), encoding="utf-8", errors="replace").read())
        for line in text.split("\n"):
            for match in call_re.finditer(line):
                per_verb["vkCmd" + match.group(1)] += 1
                per_file[rel] += 1
        if include_re.search(text):
            with_vulkan_header.append(rel)

    total = sum(per_verb.values())
    print(f"engine files: {len(files)}   including a Vulkan header: {len(with_vulkan_header)}")
    print(f"vkCmd* CALL SITES: {total} over {len(per_verb)} spelling(s)")
    print()
    for verb, count in per_verb.most_common():
        print(f"  {count:4}  {verb}")
    print()
    print("files still calling a vkCmd*:")
    for rel, count in per_file.most_common():
        print(f"  {count:4}  {rel}")
    print()
    print("files including a Vulkan header (the sweep target is <= 7, the escape bucket):")
    for rel in with_vulkan_header:
        print(f"        {rel}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
