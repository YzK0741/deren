#!/usr/bin/env python3
"""The native-handle boundary gate.

WHY THIS EXISTS, AND WHAT IT IS NOT: the goal is backend portability - a second backend (D3D12, null, ...) must
be pluggable - so the ENGINE must not call or name the graphics API. The hard evidence for "it does not" is not
a grep; it is (a) the executable's IMPORT TABLE, (b) the UNRESOLVED SYMBOLS of the engine's object files, and
(c) the source vocabulary, in that order of strength. This script reports all three and, with `--require-zero`,
fails while any of them is non-empty.

THE ENGINE SCOPE is derived from the build system rather than hand-listed: every `.cppm`/`.cpp` under `runtime/`
or `vulkan/` that is NOT one of the graphics plugins' sources (parsed out of their `target_sources`
blocks in CMakeLists.txt) and NOT in ALLOWED below.

ALLOWED is small and each entry has to say why:
  - `vulkan/constant_init/**`, `vulkan/render_layout/**`: constexpr builders and tables. They name Vulkan
    TYPES and MACROS, which is header-only work, and they compile into BOTH halves through the
    `vulkan_constant_init` target. They contain no API CALLS - which check 2 (unresolved symbols) verifies
    independently, so this entry cannot hide one.

USAGE
  python scripts/check_native_boundary.py                       # report (exit 0)
  python scripts/check_native_boundary.py --require-zero        # fail while anything is left
  python scripts/check_native_boundary.py --build-dir build-release-dyn-clang64
"""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent

# The steps that DO NOT COUNT against the engine, with the reason (see the module docstring).
ALLOWED = (
    "vulkan/constant_init/",
    "vulkan/render_layout/",
)

# Sources of the SMALL STATIC LIBRARIES the DLL links (they are compiled into `deren_vulkan.dll` even though
# they are not in its own `target_sources` block). Read from CMakeLists.txt the same way the DLL's list is, so
# a file that joins or leaves one of those targets cannot drift out of this gate.
DLL_LINKED_TARGETS = ("vulkan_error_tables", "vulkan_constant_init")
PLUGIN_TARGETS = ("deren_vulkan", "deren_gui_vulkan")

# What "touching the API" means in source: a TYPE, a MACRO, an entry point, the escape's native-handle door, or
# the header itself. Comments are stripped first - this repo's comments discuss Vulkan constantly and on purpose.
TOKEN_PATTERNS = (
    ("type", re.compile(r"\bVk[A-Z][A-Za-z0-9_]*")),
    ("macro", re.compile(r"\bVK_[A-Z0-9_]+")),
    ("entry point", re.compile(r"#include\s*<vulkan/")),  # the header, as its own category
)

# The escape's own door: `escape()->native_x(...)`, and the engine's wrappers around it (`native_x_of`).
ESCAPE_PATTERN = re.compile(r"(?:escape\(\)\.|escape->|escape_->)\s*(native_[a-z_]+|get_basis|shader_group_handles)")

BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.DOTALL)
LINE_COMMENT = re.compile(r"//[^\n]*")


def strip_comments(text: str) -> str:
    return LINE_COMMENT.sub(" ", BLOCK_COMMENT.sub(" ", text))


def target_sources(target: str) -> set[str]:
    """The source files of one CMake target, read out of its `target_sources(<target>` block.

    Parsed rather than hard-coded so this gate cannot drift from the build: if a file joins the DLL's source
    list, it leaves this script's scope in the same edit.
    """
    text = (REPO / "CMakeLists.txt").read_text(encoding="utf-8", errors="replace")
    start = text.find(f"target_sources({target}")
    if start < 0:
        return set()
    depth = 0
    end = len(text)
    for index in range(text.find("(", start), len(text)):
        if text[index] == "(":
            depth += 1
        elif text[index] == ")":
            depth -= 1
            if depth == 0:
                end = index
                break
    found = set()
    for line in text[start:end].splitlines():
        line = line.strip()
        if not line.endswith((".cppm", ".cpp", ".c")) or line.startswith("#"):
            continue
        found.add(line)
    return found


def dll_sources() -> set[str]:
    """Graphics plugin sources, including the backend's linked static libraries."""
    found: set[str] = set()
    for target in (*PLUGIN_TARGETS, *DLL_LINKED_TARGETS):
        found |= target_sources(target)
    return found


def engine_sources() -> list[pathlib.Path]:
    dll = dll_sources()
    out: list[pathlib.Path] = []
    for root in ("runtime", "vulkan"):
        base = REPO / root
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix not in (".cppm", ".cpp"):
                continue
            rel = path.relative_to(REPO).as_posix()
            if rel in dll or rel.startswith(ALLOWED) or "/third_party/" in rel:
                continue
            out.append(path)
    return out


def check_tokens(verbose: bool) -> list[str]:
    violations: list[str] = []
    for path in engine_sources():
        rel = path.relative_to(REPO).as_posix()
        code = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
        hits: list[str] = []
        for label, pattern in TOKEN_PATTERNS:
            hits += [f"{label}:{m}" for m in dict.fromkeys(pattern.findall(code))]
        hits += [f"escape:{m}" for m in dict.fromkeys(ESCAPE_PATTERN.findall(code))]
        if hits:
            violations.append(f"{rel}: {len(hits)} -> {' '.join(sorted(hits))}")
            if verbose:
                print(f"  {rel}: {' '.join(sorted(hits))}")
    return violations


def check_objects(build_dir: pathlib.Path, verbose: bool) -> list[str]:
    """The strongest check that needs a build: which engine objects reference a Vulkan symbol.

    `llvm-nm --undefined-only` over the target's objects answers exactly what the linker would complain about
    once `Vulkan::Vulkan` leaves the engine's link libraries.
    """
    nm = None
    for candidate in ("llvm-nm", "llvm-nm.exe", "nm"):
        try:
            subprocess.run([candidate, "--version"], capture_output=True, check=True)
            nm = candidate
            break
        except (OSError, subprocess.CalledProcessError):
            continue
    if nm is None:
        return ["objects: no llvm-nm/nm on PATH (skipped)"]

    objects_dir = build_dir / "CMakeFiles" / "deren_engine.dir"
    if not objects_dir.is_dir():
        return [f"objects: {objects_dir} does not exist (build first, or pass --build-dir)"]

    violations: list[str] = []
    for obj in sorted(objects_dir.rglob("*.obj")):
        # A STALE OBJECT IS NOT A FINDING: the build directory keeps the objects of files that have since been
        # deleted (`vulkan/readback/readback.cpp` is the one this gate was written around), and reporting them
        # would make the gate lie about the source tree. The object's path mirrors its source's, with `.obj`
        # appended, so a missing source is skipped - a clean build removes the object anyway.
        rel = obj.relative_to(objects_dir).as_posix()
        source = REPO / rel[: -len(".obj")]
        if not source.exists():
            continue
        result = subprocess.run([nm, "--undefined-only", str(obj)], capture_output=True, text=True)
        symbols = []
        for line in result.stdout.splitlines():
            name = line.strip().split()[-1] if line.strip() else ""
            name = name.lstrip("_")
            if re.match(r"^vk[A-Z]", name):
                symbols.append(name)
        if symbols:
            violations.append(f"{obj.name}: {' '.join(sorted(set(symbols)))}")
            if verbose:
                print(f"  {obj.name}: {' '.join(sorted(set(symbols)))}")
    return violations


def check_imports(executable: pathlib.Path, verbose: bool) -> list[str]:
    """The executable's static import table must not name the graphics API's loader."""
    if not executable.exists():
        return [f"imports: {executable} does not exist (build first, or pass --exe)"]
    objdump = None
    for candidate in ("objdump", "objdump.exe", "llvm-objdump"):
        try:
            subprocess.run([candidate, "--version"], capture_output=True, check=True)
            objdump = candidate
            break
        except (OSError, subprocess.CalledProcessError):
            continue
    if objdump is None:
        return ["imports: no objdump on PATH (skipped)"]

    result = subprocess.run([objdump, "-p", str(executable)], capture_output=True, text=True)
    imported = [line.split(":", 1)[1].strip() for line in result.stdout.splitlines() if line.strip().startswith("DLL Name:")]
    vk = [name for name in imported if "vulkan" in name.lower()]
    if verbose:
        print(f"  {executable.name} imports: {' '.join(sorted(imported))}")
    return [f"imports: {executable.name} imports {name}" for name in vk]


def main() -> int:
    parser = argparse.ArgumentParser(description="native-handle boundary gate")
    parser.add_argument("--require-zero", action="store_true", help="exit 1 while any check is non-empty")
    parser.add_argument("--verbose", action="store_true", help="print every hit, not just the counts")
    parser.add_argument("--build-dir", default="build-release-dyn-clang64", help="where the engine target's objects live")
    parser.add_argument("--exe", default=None, help="the executable whose import table is checked (default: <build-dir>/deren.exe)")
    args = parser.parse_args()

    build_dir = (REPO / args.build_dir).resolve()
    executable = pathlib.Path(args.exe).resolve() if args.exe else build_dir / "deren.exe"

    print("=== P-Census: the engine's source vocabulary ===")
    tokens = check_tokens(args.verbose)
    print(f"engine sources with graphics-API vocabulary: {len(tokens)}")
    for line in tokens:
        print(f"  {line}")

    print("=== P-Nm: the engine target's unresolved Vulkan symbols ===")
    objects = check_objects(build_dir, args.verbose)
    print(f"engine objects referencing a Vulkan symbol: {len(objects)}")
    for line in objects:
        print(f"  {line}")

    print("=== P-Import: the executable's import table ===")
    imports = check_imports(executable, args.verbose)
    print(f"graphics-API imports in {executable.name}: {len(imports)}")
    for line in imports:
        print(f"  {line}")

    remaining = len(tokens) + len(objects) + len(imports)
    if remaining and args.require_zero:
        print(f"native-boundary gate: FAIL ({remaining} findings)")
        return 1
    print(f"native-boundary gate: {'PASS' if remaining == 0 else f'{remaining} findings (report-only)'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
