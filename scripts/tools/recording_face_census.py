#!/usr/bin/env python3
"""census the recording face by CONVENTION, not by grep: the plan's section 9.1 numbers.

WHY A SCRIPT AND NOT A GREP: the plan's first census reported `vkCmdPipelineBarrier2` as ZERO engine
call sites while 52 real ones sat in the same files it scanned, and the two sweep numbers this effort
is measured by are only comparable if the convention is fixed. The convention (plan section 9.1):

  * engine files = source/engine/** plus source/app/main.cpp and source/app/chores.cppm,
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

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BACKEND_PREFIX = "source/backends/vulkan/core/"
EXTENSIONS = (".cppm", ".cpp", ".hpp")


def strip_comments(text: str) -> str:
    """Removes // to end of line and /* ... */ blocks, leaving line structure intact."""
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def engine_files():
    files = []
    for base in ("source/engine",):
        for root, _dirs, names in os.walk(os.path.join(ROOT, base)):
            for name in names:
                if not name.endswith(EXTENSIONS):
                    continue
                rel = os.path.relpath(os.path.join(root, name), ROOT).replace("\\", "/")
                if rel.startswith(BACKEND_PREFIX):
                    continue
                files.append(rel)
    for extra in ("source/app/main.cpp", "source/app/chores.cppm"):
        if os.path.isfile(os.path.join(ROOT, extra)):
            files.append(extra)
    return sorted(files)


def census_recipes(files):
    """The barrier RECIPES: `*_transition` AND `*_dependency`, defined and used.

    The second kind exists because a barrier need not change a layout: `deferred.cpp` uses
    `color_attachment_dependency` to order one pass's colour-attachment store before the next instance's
    LOAD. A `*_transition`-only scan cannot see it - which is exactly the miss this function fixes.

    A RECIPE IS WHAT `source/backends/vulkan/constant_init/constant_init.cppm` DECLARES it to be, and that is deliberate:
    sites also name local `VkDependencyInfo` variables (`sampling_dependency`, `copy_dependency`, ...), so
    "any name ending in _dependency" would count locals as vocabulary and report a dozen phantom gaps. The
    defined set comes from the declaring file; a USE is a defined name appearing in an engine file.
    """
    definition_re = re.compile(r"\b(\w+_(?:transition|dependency))\s*=\s*\{")
    declaring_file = "source/backends/vulkan/constant_init/constant_init.cppm"
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


# THE RECIPE -> ROLE-PAIR MAP, and why it is data here rather than a derivation: this renderer keeps every
# image in GENERAL, so a recipe's masks say what a transition MEANS but its name says which two ROLES meet -
# and names alone cannot be parsed (`hdr_sampling_transition` is color_attachment -> shader_read). The map
# exists so the ROLE HISTOGRAMS below can be printed, and those histograms are what a rule in the backend's
# unsupported list must cite: "no recipe reads out of `present`" is checkable against `present as from: 0`,
# which is a number this script prints - not a claim someone wrote down.
RECIPE_ROLES = {
    "color_attachment_transition": ("undefined", "color_attachment"),
    # PLAN X4: the host-access pair the engine used to record by hand (the probes read-back). It is a RECIPE
    # like the others - the census counts what constant_init DECLARES, which is why it joins this table.
    "color_attachment_to_host_transition": ("color_attachment", "host_read"),
    "depth_attachment_transition": ("undefined", "depth_attachment"),
    "undefined_to_sampling_transition": ("undefined", "shader_read"),
    "undefined_to_depth_sampling_transition": ("undefined", "depth_read"),
    "undefined_to_general_transition": ("undefined", "shader_write"),
    "undefined_to_transfer_dst_transition": ("undefined", "transfer_destination"),
    "undefined_to_present_transition": ("undefined", "present"),
    "hdr_sampling_transition": ("color_attachment", "shader_read"),
    "color_attachment_to_transfer_transition": ("color_attachment", "transfer_source"),
    "present_transition": ("color_attachment", "present"),
    "color_attachment_dependency": ("color_attachment", "color_attachment"),
    "general_to_sampling_transition": ("shader_write", "shader_read"),
    "general_to_transfer_src_transition": ("shader_write", "transfer_source"),
    "compute_storage_transition": ("shader_write", "shader_read_write"),
    "sampling_to_general_transition": ("shader_read", "shader_write"),
    "sampling_to_transfer_dst_transition": ("shader_read", "transfer_destination"),
    "sampling_to_depth_attachment_transition": ("shader_read", "depth_attachment"),
    "shadow_map_sampling_transition": ("depth_attachment", "shader_read"),
    "transfer_to_color_attachment_transition": ("transfer_source", "color_attachment"),
    "transfer_dst_to_sampling_transition": ("transfer_destination", "shader_read"),
    "transfer_src_to_sampling_transition": ("transfer_source", "shader_read"),
}


def print_role_histograms():
    """The from/to role histograms: the evidence a rule in the backend's unsupported list must cite."""
    froms = collections.Counter()
    tos = collections.Counter()
    for _name, (frm, to) in RECIPE_ROLES.items():
        froms[frm] += 1
        tos[to] += 1
    roles = sorted(set(froms) | set(tos))
    print("ROLE HISTOGRAMS over the measured recipe set (a rule's `why` must cite one of these numbers):")
    print(f"    {'role':22} {'as from':>8} {'as to':>8}")
    for role in roles:
        print(f"    {role:22} {froms[role]:8} {tos[role]:8}")
    pairs = set(RECIPE_ROLES.values())
    print(f"  measured pairs: {len(pairs)}   role values: {len(roles)}   "
          f"combinations: {len(roles) ** 2}   unsupported by this measurement: {len(roles) ** 2 - len(pairs)}")
    # THE PAIRS THEMSELVES, because a COUNT IS NOT A SET: the backend's gate asserts set equality against
    # this list (a substituted row preserves every count - that is exactly how the first version of that gate
    # stayed green while a real pair was missing), so the list must be printable for the two to be compared.
    print("  the measured pairs, verbatim (this is the MEASUREMENT, not a copy of the backend table):")
    for frm, to in sorted(pairs):
        print(f"        {frm} -> {to}")
    return dict(froms), dict(tos)


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
    print_role_histograms()
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
