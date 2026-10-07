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


def census_recipes(files):
    """The barrier RECIPES: `*_transition` AND `*_dependency`, defined and used.

    The second kind exists because a barrier need not change a layout: `deferred.cpp` uses
    `color_attachment_dependency` to order one pass's colour-attachment store before the next instance's
    LOAD. A `*_transition`-only scan cannot see it - which is exactly the miss this function fixes.

    A RECIPE IS WHAT `vulkan/constant_init/constant_init.cppm` DECLARES it to be, and that is deliberate:
    sites also name local `VkDependencyInfo` variables (`sampling_dependency`, `copy_dependency`, ...), so
    "any name ending in _dependency" would count locals as vocabulary and report a dozen phantom gaps. The
    defined set comes from the declaring file; a USE is a defined name appearing in an engine file.
    """
    definition_re = re.compile(r"\b(\w+_(?:transition|dependency))\s*=\s*\{")
    declaring_file = "vulkan/constant_init/constant_init.cppm"
    result = {
        "transition": {"defined": set(), "used": set()},
        "dependency": {"defined": set(), "used": set()},
    }
    declaring = strip_comments(open(os.path.join(ROOT, declaring_file), encoding="utf-8", errors="replace").read())
    for name in definition_re.findall(declaring):
        kind = "transition" if name.endswith("_transition") else "dependency"
        result[kind]["defined"].add(name)
    for rel in files:
        text = strip_comments(open(os.path.join(ROOT, rel), encoding="utf-8", errors="replace").read())
        for name in re.findall(r"\b\w+_(?:transition|dependency)\b", text):
            kind = "transition" if name.endswith("_transition") else "dependency"
            if name in result[kind]["defined"]:
                result[kind]["used"].add(name)
    return result


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
    # ---- THE OTHER HALF OF THE MEASUREMENT: WHICH BARRIER SHAPES EXIST AT ALL -------------------
    # FOUND BY A MISS: this script used to count call sites only, so the RECIPE a site uses was invisible -
    # and `deferred.cpp` orders a colour-attachment store before this instance's load with a
    # `color_attachment_dependency`, a shape no `vkCmd*` count can show and no `*_transition` scan finds.
    # Both recipe kinds are therefore counted and reported SEPARATELY, together with the recipes that are
    # DEFINED BUT NOT YET USED (the vocabulary the backend's pair table still owes) and, as a guard, any
    # recipe a site names that is not defined at all.
    recipes = census_recipes(files)
    print("BARRIER RECIPES (the shapes a call-site count cannot show):")
    print(f"  transition recipes: {len(recipes['transition']['defined'])} defined, "
          f"{len(recipes['transition']['used'])} used by engine sites")
    print(f"  dependency recipes: {len(recipes['dependency']['defined'])} defined, "
          f"{len(recipes['dependency']['used'])} used by engine sites")
    for kind in ("transition", "dependency"):
        unused = sorted(recipes[kind]["defined"] - recipes[kind]["used"])
        if unused:
            print(f"  {kind} recipes DEFINED BUT NOT USED (the vocabulary still owed - if any of these are "
                  f"not measurable, they are dead):")
            for name in unused:
                print(f"        {name}")
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
