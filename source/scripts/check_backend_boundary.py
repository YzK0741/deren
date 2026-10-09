#!/usr/bin/env python3
"""check_backend_boundary.py - the FLIP GATE, measured from the link artifacts.

WHAT THIS IS
    `dynamic_link` can only replace load-time binding once nothing on the engine side
    references a symbol that `deren_vulkan` defines. That set - not the number of `vk*`
    spellings in the source - is what has to reach zero before the flip. This script
    measures exactly that set:

        symbols DEFINED in deren_vulkan  INTERSECT  symbols UNDEFINED in engine/application consumers

    and it measures it from the ARCHIVES, not from the source and not from the object
    directories. The object directories are not trustworthy here: `build-release-clang64/
    CMakeFiles/deren_engine.dir` still holds pre-split objects from before S1-A, and a
    directory-level count reports 130 where the archive-level truth is 78.

WHY A RATCHET
    The same discipline as the 13 frozen render hashes: a checked-in baseline, and the
    gate fails when a NEW SYMBOL appears, even if another dependency disappeared.
    While the migration is in flight the count falls;
    lower the baseline with `--update` each time, and the boundary can never quietly
    re-acquire a dependency, because `--update` refuses a "joiner" (a symbol the ratchet
    never recorded).

THE FLIP GATE IS WHITELIST-AWARE (`--require-zero`)
    The archive boundary cannot reach zero: some symbols are deliberate exceptions. So the
    flip gate asks a different question - NOTHING OUTSIDE THE WHITELIST. The whitelist lives
    in `source/scripts/backend_boundary_whitelist.json`: one entry per symbol, with the reason it is
    still there and what removes it, printed on every run. It is SHRINK-ONLY in three
    enforceable senses:
      * a measured symbol outside the whitelist fails (the list is not an allow-all);
      * a whitelist entry with no live hit fails (the exception it claims is gone);
      * an entry the ratchet never tracked fails (a new dependency cannot be whitelisted
        into existence - `--update` refuses joiners anyway).
    `--update` never writes the whitelist: only a human edits it.

THE THIRD INSTRUMENT: THE IMPORT GRAPH
    A symbol count can reach zero while the engine keeps importing the backend's MODULES - and
    a module import is what actually forbids a SHARED backend, because the BMI would have to
    come from the DLL. `--require-zero` therefore also scans the engine/application sources and
    fails on every import of a module `deren_vulkan` owns. Ownership comes from CMake's own
    `target_sources(deren_vulkan ...)` list, NOT from a name prefix: `source/engine/filters/
    filters.cppm` declares `deren.engine.filters` but belongs to the ENGINE, while
    `source/backends/vulkan/constant_init/constant_init.cppm` belongs to the BACKEND. Test sources are reported
    separately (they link the backend deliberately) and are not part of the gate.

THE SECOND NUMBER: OWNING STL ACROSS THE BOUNDARY
    A symbol whose signature carries `std::vector` / `std::string` / an allocator is an
    allocation that one image makes and the other frees (today:
    `init_utils::create_host_buffers(..., std::vector<vk_buffer>&, ...)`). The contract
    forbids it (no STL across the boundary), so the count is tracked
    as a ratchet of its own - it is the one part of the migration where "it links" is not
    the same as "it is safe".

HISTORICAL MEASUREMENT, build-release-clang64 (Release clang64):
    78 symbols / 227 reference sites / 31 archive members; 0 in the reverse direction;
    1 of the 78 carries owning STL.

BASELINES ARE PER TOOLCHAIN (the symbol sets are not comparable across them):
    backend_boundary_baseline.mingw.json   <- the measured one, from the clang64 tree
    backend_boundary_baseline.msvc.json    <- record before gating the MSVC tree
    backend_boundary_baseline.posix.json   <- same for a Linux tree
    A tree with no baseline fails; `--initialize` explicitly records the first one.
    `--update` can only reduce an existing set. Neither mutation obeys `--warn`.

ONE CONFIGURATION, ONE PAIR (S5, the flip)
    Step 2 built a SECOND runtime (`source/engine/runtime/`, contract-only) in its own tree, and while both existed the
    two trees crossed the boundary in DIFFERENT SHAPES: the legacy engine referenced the CONCRETE CLASS
    (`core::core(create_info const&)`), the dynamic one references the C ENTRY
    (`deren_make_api_core` / `deren_destroy_api_core`). Sharing one pair of files would have made each
    tree report the other's symbols as STALE - noise, not a boundary - so each configuration carried its
    OWN baseline and whitelist, both shrink-only, each with its reasons.

    S5 DELETED THE LEGACY RUNTIME (`source/engine/runtime/`, which named `core`) and the legacy pair with it.
    What is left is the DYNAMIC configuration, whose boundary is the C entry - the DESIGNED boundary,
    declared in source/promise/rhi/backend_entry.hpp, and the one that never leaves while the backend is a DLL:

        --config dynamic   (default, the only value)  backend_boundary_baseline.dynamic.<flavor>.json
                                                     backend_boundary_whitelist_dynamic.json

    `--config` is kept as an accepted argument so the invocations the migration's docs and CI already
    carry keep working; `--config legacy` is now a named argument error rather than a silent fallback,
    and the default `--build-dir` is the surviving tree. `--build-dir` still overrides it, and WHICH SET
    IS BEING MEASURED IS PRINTED ON EVERY RUN, `--quiet` INCLUDED: a blind quiet run once read as a
    dynamic-tree measurement while it was measuring the legacy pair again, so the identification is
    deliberately not part of the quiet-able report.

USAGE
    python source/scripts/check_backend_boundary.py                       # gate against the baseline
    python source/scripts/check_backend_boundary.py --update              # ratchet down to today
    python source/scripts/check_backend_boundary.py --list                # the worklist, demangled, + the imports
    python source/scripts/check_backend_boundary.py --warn                # ordinary checks report failures; mutations/flip stay strict
    python source/scripts/check_backend_boundary.py --require-zero        # the flip gate: whitelist-aware + import graph
    python source/scripts/check_backend_boundary.py --config dynamic      # explicit spelling of the only value
    python source/scripts/check_backend_boundary.py --build-dir DIR
    python source/scripts/check_backend_boundary.py --whitelist PATH      # alternate whitelist (tests)
    python source/scripts/check_backend_boundary.py --repo-root DIR       # alternate tree for the import scan (tests)
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from collections import defaultdict

BACKEND_TARGET = "deren_vulkan"
KIT_TARGET = "deren_engine"

# Where the two halves land, per toolchain. MinGW/clang64 archives first, MSVC after.
#
# THE BACKEND IS A DLL SINCE THE SHARED FLIP, so its public surface is its EXPORT TABLE rather than
# an archive's symbol table: a `nm` over the DLL would read the (stripped) image symbol table, which
# says nothing about what the library OFFERS. `BACKEND_DLL_NAMES` is resolved first, and the export
# table (llvm-readobj --coff-exports / objdump -p) becomes the "defined in the backend" set. The
# archive spellings stay for a STATIC tree, so the gate still measures an intermediate state.
ARCHIVE_NAMES = {
    BACKEND_TARGET: ("libderen_vulkan.a", "deren_vulkan.lib"),
    KIT_TARGET: ("libderen_engine.a", "deren_engine.lib"),
}
BACKEND_DLL_NAMES = ("deren_vulkan.dll", "libderen_vulkan.dll")
# The process-wide half, which the backend DLL must import (batch ④'s package: the backend ships as
# deren_vulkan.dll + shared_utility.dll).
SHARED_UTILITY_DLL = "shared_utility.dll"
UTILITY_SHARED_DLL = SHARED_UTILITY_DLL

# The nm to use. llvm-nm reads both GNU archives and MSVC .lib; plain nm is the fallback.
NM_CANDIDATES = ("llvm-nm", "llvm-nm-18", "llvm-nm-17", "nm")
FILT_CANDIDATES = ("llvm-cxxfilt", "llvm-cxxfilt-18", "c++filt")

# Reference sites are attributed to an area by the member name they come from. This is
# reporting only; the gate itself is the SET of symbols.
AREA_PATTERNS = (
    ("runtime", re.compile(r"^runtime")),
    ("pass", re.compile(r"^(pass|taa|scene|transparent|character_forward|toon_screen_rim|"
                        r"goo_rim|megalights|ray_traced_shadow|mask_bake|compute_skin|cluster|"
                        r"deferred|post|fxaa|upscale|geometry_buffer_debug|shadow|chain)")),
)

# An allocation that crosses the boundary: libc++'s std::vector / std::string / allocator,
# and their libstdc++ spellings, appear in the MANGLED name of any symbol that passes one.
OWNING_STL_RE = re.compile(r"(6vector|12basic_string|9allocator|6string)")


def category(symbol: str) -> str:
    if re.search(r"13vma_allocator", symbol):
        return "core::vma_allocator"
    if re.search(r"15descriptor_heap", symbol):
        return "core::descriptor_heap"
    if re.search(r"10init_utils", symbol):
        return "init_utils"
    if re.search(r"8pipeline", symbol):
        return "core::pipeline"
    if re.search(r"7filters", symbol):
        return "core::filters"
    # The generic handle bucket comes LAST of the specific ones but before core::core:
    # a handle symbol's signature often spells `core` as well.
    if re.search(r"7handles", symbol) or re.search(r"(8vk_image|9vk_buffer)", symbol):
        return "native RAII handle (vk_*)"
    if re.search(r"4core4core", symbol):
        return "core::core member"
    if symbol.startswith("_ZGI") or symbol.startswith("_ZGV"):
        return "data: init/guard"
    return "other"


def carries_owning_stl(symbol: str) -> bool:
    return OWNING_STL_RE.search(symbol) is not None


def find_tool(candidates) -> str | None:
    for name in candidates:
        path = shutil.which(name)
        if path:
            return path
    return None


def same_artifact(left: str, right: str) -> bool:
    # 路径别名和硬链接都不能把已知归档冒充成另一种消费者。
    if os.path.normcase(os.path.realpath(left)) == os.path.normcase(os.path.realpath(right)):
        return True
    try:
        return os.path.samefile(left, right)
    except OSError:
        return False


def resolve_archive(build_dir: str, target: str) -> str:
    for name in ARCHIVE_NAMES[target]:
        candidate = os.path.join(build_dir, name)
        if os.path.isfile(candidate):
            return candidate
    searched = ", ".join(ARCHIVE_NAMES[target])
    raise SystemExit(
        f"check_backend_boundary: no archive for '{target}' in {build_dir!r} "
        f"(looked for {searched}); build the tree first, or pass --build-dir"
    )


def platform_flavor() -> str:
    """The toolchain key when there is no build tree to inspect (the `--config` default path)."""
    return "mingw" if sys.platform.startswith("win") else "posix"


def flavor_of(build_dir: str | None) -> str:
    """Which baseline this tree's symbol sets are comparable with.

    The mangled names, and therefore the counts, differ per toolchain and per standard
    library, so the baseline is keyed rather than shared.
    """
    if build_dir is None:
        return platform_flavor()
    if any(os.path.isfile(os.path.join(build_dir, name)) for name in ARCHIVE_NAMES[BACKEND_TARGET][1:]):
        return "msvc"
    return platform_flavor()


# WHICH RUNTIME CONFIGURATION A RUN MEASURES (`--config`). ONE ENTRY SINCE S5: the flip deleted the
# legacy runtime (`source/engine/runtime/`) and the legacy pair of files with it, so there is one boundary shape
# left - the C entry, the designed boundary declared in source/promise/rhi/backend_entry.hpp. The argument is
# kept (not removed) because the invocations the migration's docs and CI carry pass `--config dynamic`,
# and `choices=` makes `--config legacy` a NAMED refusal rather than a silent fallback to another pair.
# `{flavor}` is filled by `flavor_of(build_dir)` (the toolchain key of the symbol sets).
CONFIGURATIONS = {
    "dynamic": {
        "description": "the runtime: the boundary is the ONE C ENTRY (deren_make_api_core, abi 18) - "
                       "the backend is a DLL and the executable resolves it by name",
        "baseline": "backend_boundary_baseline.dynamic.{flavor}.json",
        "whitelist": "backend_boundary_whitelist_dynamic.json",
        "build_dir": "build-release-dyn-clang64",
    },
}


MEMBER_RE = re.compile(r"^(?P<member>[^:\[\]]+\.(?:obj|o)):\s*$")


# ---- (b) THE C++ RUNTIME PREMISE OF BATCH ④ (the utility split) -------------------------------
# The split's relaxed export rules (STL across the seam, a `log(fmt, args...)` template in a module
# interface) rest on ONE measured fact: the executable imports libc++.dll and uses the UCRT heap, so
# both images share one C++ runtime and one heap. THAT FACT IS A BUILD CONFIGURATION, NOT A LAW - the
# tree's older shape linked `-static`, which pins libc++ INTO the image and gives it a private runtime
# and a private heap; under that configuration an allocation on one side of the seam and a free on the
# other is undefined. So it is checked on the artifacts here, rather than assumed.
CPP_RUNTIME_DLL = "libc++.dll"
# Every image that must import it, WHEN IT EXISTS. `deren_vulkan.dll` is the backend's SHARED spelling
# (abi 18): the rule is symmetric, so the day that DLL is built this check covers it with no edit.
# `deren_assets.dll` joined in the binary-balance batch: the asset-loading stack is a third image with
# its own copy of the C++ runtime and its own calls into the process-wide sink, so it is held to the
# same two premises as the other two (measured on it: libc++.dll + shared_utility.dll both imported).
CPP_RUNTIME_IMAGES = ("deren.exe", "deren_vulkan.dll", "deren_assets.dll")


def image_dll_imports(image: str) -> list[str] | None:
    """The DLL names an image imports, or None when no objdump could be run."""
    for tool in ("llvm-objdump", "objdump"):
        path = shutil.which(tool)
        if path is None:
            continue
        proc = subprocess.run([path, "-p", image], capture_output=True, text=True)
        if proc.returncode != 0:
            continue
        names = re.findall(r"DLL Name:\s*(\S+)", proc.stdout)
        if names:
            return names
    return None


def check_cpp_runtime(build_dir: str) -> dict:
    """Which built images import the DYNAMIC C++ runtime, and which do not.

    A STATIC libc++ leaves NO trace of itself in an import table - that ABSENCE is the failure mode
    this looks for, which is why the question is "is the dynamic runtime imported", never "was a
    static runtime symbol found". The synthetic trees the unit tests build have no image at all; there
    the check reports an empty `images` map and the flip gate's application-evidence rule is what
    demands a real one.
    """
    images: dict[str, dict] = {}
    for name in CPP_RUNTIME_IMAGES:
        path = os.path.join(build_dir, name)
        if not os.path.isfile(path):
            continue
        imports = image_dll_imports(path)
        images[name] = {"imports": imports, "dynamic_cxx_runtime": bool(imports and CPP_RUNTIME_DLL in imports)}
    return {"ok": all(entry["dynamic_cxx_runtime"] for entry in images.values()),
            "dll": CPP_RUNTIME_DLL, "images": images}


# ---- (a) ONE COPY OF THE PROCESS-WIDE HALF, ON THE ARTIFACTS --------------------------------
# The utility split's subject is STATE: the log sink must exist once per process. In this batch both
# halves are STATIC, so "once" is a property of the LINK - and the link is decided by what the archives
# carry. Two questions, both answerable from the archives alone:
#   1. DOES A SECOND COPY OF THE SHARED TUs EXIST? `libshared_utility.a` owns them; if the engine or
#      the backend archive also carried `shared_utility.cpp.obj` (or better_pmr / log_rotation_claim),
#      then two copies of the sink are in the tree, whatever the link does with them.
#   2. DOES THE OTHER HALF GO TO IT? The engine and backend archives must REFERENCE symbols this one
#      archive DEFINES (their `log` / `panic` / `init_pmr` call sites). A half that references none is
#      either not using the toolkit or carrying its own copy - and question 1 tells the two apart.
# This is the batch's own witness (a): the engine archive and the backend archive reference the SAME
# shared utility rather than each having one, checked by member names + undefined/defined symbol sets
# (`nm --defined-only` / `--undefined-only` over the four archives), not by reading the link line.
UTILITY_SHARED_ARCHIVE = "libshared_utility.a"
# Every other archive that links the toolkit; the two halves plus the engine library.
UTILITY_SHARED_CONSUMERS = ("libstatic_utility.a", "libderen_engine.a", "libderen_vulkan.a")
# The TUs that belong to the process-wide half. If one of these member names shows up in another
# archive, that archive built its own copy of the shared code (the exact thing the split prevents).
UTILITY_SHARED_MEMBERS = ("shared_utility.cpp.obj", "shared_utility.cppm.obj", "better_pmr.cpp.obj",
                          "log_rotation_claim.cpp.obj")


def check_shared_utility(build_dir: str, nm: str) -> dict:
    """(a) ONE COPY OF THE PROCESS-WIDE HALF, in whichever shape this tree has.

    STATIC REGIME (before the flip): the shared TUs live in `libshared_utility.a`, and "once per
    process" is a property of the LINK - so the engine and the backend archives must not carry their own
    copy, and each must reference symbols that archive defines.

    DLL REGIME (the flip, and the regime the split was for): `shared_utility.dll` IS the one copy,
    because the OS loads one image per process - so the evidence is that EVERY consumer imports it
    (`deren.exe` and `deren_vulkan.dll`), and that no archive in the tree carries the shared TUs as
    members. That is the property the split exists to guarantee, checked on the artifacts rather than
    asserted: two copies would show up as a second importer-visible image or as the TUs inside an
    archive that gets linked into one of the two images.
    """
    dll_path = os.path.join(build_dir, UTILITY_SHARED_DLL)
    result: dict = {
        "ok": True,
        "archive": UTILITY_SHARED_ARCHIVE,
        "dll": None,
        "defined_symbols": 0,
        "shared_members": [],
        "copies": [],
        "referenced_by": {},
    }
    if os.path.isfile(dll_path):
        result["dll"] = UTILITY_SHARED_DLL
        exports = read_backend_exports(dll_path) or []
        result["defined_symbols"] = len(exports)
        # Every consumer in the package must import the same image.
        for consumer in CPP_RUNTIME_IMAGES:
            path = os.path.join(build_dir, consumer)
            if not os.path.isfile(path):
                continue
            imports = image_dll_imports(path)
            result["referenced_by"][consumer] = 1 if (imports and UTILITY_SHARED_DLL in imports) else 0
        # ... and no archive may carry the process-wide TUs.
        for name in UTILITY_SHARED_CONSUMERS:
            path = os.path.join(build_dir, name)
            if not os.path.isfile(path):
                continue
            _, members = read_symbols(path, nm, defined=True)
            duplicates = sorted(member for member in UTILITY_SHARED_MEMBERS if member in members)
            if duplicates:
                result["copies"].append({"archive": name, "members": duplicates})
        result["ok"] = (not result["copies"]) and bool(result["referenced_by"]) and all(
            count > 0 for count in result["referenced_by"].values())
        return result

    shared_path = os.path.join(build_dir, UTILITY_SHARED_ARCHIVE)
    if not os.path.isfile(shared_path):
        # no shared artifact in this tree (a synthetic tree, or a build that never linked the toolkit):
        # nothing to judge, and the flip gate's application evidence is what demands a real one.
        return result
    shared_defined, shared_members = read_symbols(shared_path, nm, defined=True)
    result["defined_symbols"] = len(shared_defined)
    result["shared_members"] = sorted(member for member in shared_members if member in UTILITY_SHARED_MEMBERS)
    for name in UTILITY_SHARED_CONSUMERS:
        path = os.path.join(build_dir, name)
        if not os.path.isfile(path):
            continue
        _, members = read_symbols(path, nm, defined=True)
        duplicates = sorted(member for member in UTILITY_SHARED_MEMBERS if member in members)
        if duplicates:
            result["copies"].append({"archive": name, "members": duplicates})
        consumer_undefined, _ = read_symbols(path, nm, defined=False)
        result["referenced_by"][name] = len(set(consumer_undefined) & set(shared_defined))
    result["ok"] = (not result["copies"]) and all(count > 0 for count in result["referenced_by"].values())
    return result


def read_backend_exports(dll: str) -> list[str] | None:
    """The names a DLL EXPORTS, or None when neither objdump could read them.

    An export table is the honest subject once a library is a DLL: the archive's symbol table is gone
    (and a Release image is stripped anyway), while the export table is exactly what a host can resolve
    at run time - and exactly what must be checked to keep the boundary's surface designed rather than
    incidental.
    """
    for tool, flag in (("llvm-readobj", "--coff-exports"), ("objdump", "-p")):
        path = shutil.which(tool)
        if path is None:
            continue
        proc = subprocess.run([path, flag, dll], capture_output=True, text=True)
        if proc.returncode != 0:
            continue
        if tool == "llvm-readobj":
            names = re.findall(r"^\s*Name:\s*(\S+)\s*$", proc.stdout, re.MULTILINE)
        else:
            # objdump -p prints an "Export Address Table" then a bracketed name table; the exported names
            # are the bracketed entries after that header.
            tail = proc.stdout.split("[Ordinal/Name Pointer] Table", 1)
            names = re.findall(r"^\s*\[\s*\d+\]\s*(\S+)\s*$", tail[1], re.MULTILINE) if len(tail) == 2 else []
        if names:
            return names
    return None


def check_dll_surface(build_dir: str) -> dict:
    """(the flip's own instrument) WHAT THE BACKEND DLL OFFERS, AND WHICH HALVES ARE IN PLACE.

    Three questions, all on the artifact rather than on the link line:
      1. the export table carries EXACTLY the one designed entry - `deren_make_api_core`. A second
         exported name means something else became part of the ABI by accident (an auto-export setting,
         a missing dllexport boundary, a stray `extern "C"`);
      2. the DLL imports `shared_utility.dll` - the process-wide half of the package, which is what keeps
         ONE log sink in the process (batch ④'s whole subject);
      3. the package's two halves are both there at all.
    A STATIC tree has no DLL; then the archive's symbol table is the surface and this check says so.
    """
    result: dict = {"ok": True, "dll": None, "exports": [], "imports": None, "imports_shared_utility": False}
    for name in BACKEND_DLL_NAMES:
        path = os.path.join(build_dir, name)
        if os.path.isfile(path):
            result["dll"] = name
            result["exports"] = read_backend_exports(path) or []
            result["imports"] = image_dll_imports(path)
            result["imports_shared_utility"] = bool(result["imports"] and SHARED_UTILITY_DLL in result["imports"])
            break
    if result["dll"] is None:
        return result
    result["ok"] = result["exports"] == ["deren_make_api_core"] and result["imports_shared_utility"]
    return result


def read_symbols(archive: str, nm: str, *, defined: bool) -> tuple[dict[str, list[str]], set[str]]:
    """Return ({symbol: [members]}, {members}) for one archive.

    nm's archive output interleaves member headers ("name.obj:") with the member's
    symbols, so the header line is the current member for every symbol that follows.
    """
    flag = "--defined-only" if defined else "--undefined-only"
    proc = subprocess.run([nm, flag, "--no-demangle", archive],
                          capture_output=True, text=True)
    if proc.returncode != 0:
        raise SystemExit(f"check_backend_boundary: {nm} failed on {archive}\n{proc.stderr.strip()}")

    by_symbol: dict[str, list[str]] = defaultdict(list)
    members: set[str] = set()
    member = "<archive>"
    for line in proc.stdout.splitlines():
        if not line.strip():
            continue
        header = MEMBER_RE.match(line)
        if header and " " not in line.strip():
            member = os.path.basename(header.group("member"))
            members.add(member)
            continue
        parts = line.split()
        if len(parts) < 2:
            continue
        # "                 U symbol" -> last field; "0000 T symbol" -> last field too.
        symbol = parts[-1]
        if symbol in ("U", "*"):
            continue
        if member not in by_symbol[symbol]:
            by_symbol[symbol].append(member)
    return by_symbol, members


def area_of(member: str) -> str:
    for name, pattern in AREA_PATTERNS:
        if pattern.match(member):
            return name
    return "other"


def load_baseline(path: str) -> dict | None:
    if not os.path.isfile(path):
        return None
    try:
        with open(path, encoding="utf-8") as handle:
            baseline = json.load(handle)
        symbols = baseline["cross_boundary_symbols"]
        owning = baseline["owning_stl_symbols"]
        if (not isinstance(symbols, list) or not all(isinstance(s, str) for s in symbols)
                or len(set(symbols)) != len(symbols) or baseline["count"] != len(symbols)
                or not isinstance(owning, list) or not all(isinstance(s, str) for s in owning)
                or len(set(owning)) != len(owning) or baseline["owning_stl_count"] != len(owning)
                or not set(owning).issubset(symbols)):
            raise ValueError("inconsistent symbol counts or sets")
        return baseline
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise ValueError(f"invalid baseline {path}: {error}") from error


def load_whitelist(path: str) -> dict | None:
    """The flip gate's whitelist: symbols the boundary may still carry, each with its reason.

    Shrink-only, and that is enforced rather than documented: an entry with no live hit is a failure
    in `--require-zero` (the exception it claims no longer exists, so the entry must go), the file is
    never written by `--update`, and an entry the ratchet never tracked is refused too - so a new
    dependency cannot be silenced by editing this file alone.
    """
    if not os.path.isfile(path):
        return None
    try:
        with open(path, encoding="utf-8") as handle:
            data = json.load(handle)
        entries = data["entries"]
        if (not isinstance(entries, list)
                or not all(isinstance(e, dict) and isinstance(e.get("symbol"), str) and e["symbol"]
                           and isinstance(e.get("reason"), str) and e["reason"].strip() for e in entries)
                or data["count"] != len(entries)
                or len({e["symbol"] for e in entries}) != len(entries)):
            raise ValueError("inconsistent entries: every entry needs a symbol and a non-empty reason, "
                             "symbols are unique, and count matches")
        symbols = [e["symbol"] for e in entries]
        return {"count": len(entries), "entries": entries, "symbols": symbols, "path": os.path.abspath(path)}
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise ValueError(f"invalid whitelist {path}: {error}") from error


IMPORT_RE = re.compile(r"^\s*(?:export\s+)?import\s+([\w.]+(?::[\w.]+)?)\s*;", re.M)
MODULE_DECL_RE = re.compile(r"^\s*(?:export\s+)?module\s+([\w.]+)(?::[\w.]+)?\s*;", re.M)
SOURCE_SUFFIXES = (".cppm", ".cpp", ".hpp", ".h")


def backend_modules_of(repo_root: str) -> tuple[set[str], set[str]]:
    """({modules deren_vulkan owns}, {its source files, repo-relative}).

    OWNERSHIP COMES FROM THE TARGET, NOT FROM THE NAME. `source/engine/filters/filters.cppm` declares
    `deren.engine.filters` but belongs to the ENGINE target (deren_engine), while
    `source/backends/vulkan/constant_init/constant_init.cppm` declares `deren.vulkan.constant_init` and belongs to the
    BACKEND - so a name prefix would be wrong in both directions. The list is read out of CMake's own
    `target_sources(deren_vulkan ...)` block, which is what makes this check self-maintaining.
    """
    cmake = os.path.join(repo_root, "CMakeLists.txt")
    if not os.path.isfile(cmake):
        return set(), set()
    with open(cmake, encoding="utf-8", errors="replace") as handle:
        text = handle.read()
    block = re.search(r"target_sources\(deren_vulkan\b(.*?)\n\)", text, re.S)
    if block is None:
        return set(), set()
    files = {f.replace("\\", "/") for f in re.findall(r"^\s*([\w./\\-]+\.(?:cppm|cpp|c|h|hpp))\s*$", block.group(1), re.M)}
    modules = set()
    for rel in sorted(files):
        if not rel.endswith(".cppm"):
            continue
        path = os.path.join(repo_root, rel)
        if not os.path.isfile(path):
            continue
        with open(path, encoding="utf-8", errors="replace") as handle:
            modules.update(MODULE_DECL_RE.findall(handle.read()))
    return modules, files


def scan_backend_imports(repo_root: str, backend_modules: set[str], backend_files: set[str]) -> dict:
    """Which engine/application sources still import a module deren_vulkan owns.

    THE LAYER THE SYMBOL COUNT CANNOT SEE: a symbol dependency can fall to zero while the engine keeps
    importing the backend's modules, and a module import is what makes a SHARED backend impossible
    (the BMI would have to come from the DLL). This reports the engine/application side as the gate's
    subject and the test sources separately, because the tests link the backend deliberately.
    """
    engine: list[str] = []
    tests: list[str] = []
    if not backend_modules:
        return {"engine": engine, "tests": tests, "modules": sorted(backend_modules), "scanned": 0}
    scanned = 0
    for directory, subdirectories, names in os.walk(repo_root):
        subdirectories[:] = [d for d in subdirectories
                             if d not in (".git", "third_party") and not d.startswith("build")]
        for name in names:
            if not name.endswith(SOURCE_SUFFIXES):
                continue
            absolute = os.path.join(directory, name)
            relative = os.path.relpath(absolute, repo_root).replace("\\", "/")
            if relative in backend_files:
                continue
            scanned += 1
            with open(absolute, encoding="utf-8", errors="replace") as handle:
                text = handle.read()
            for match in IMPORT_RE.finditer(text):
                imported = match.group(1)
                if imported.split(":", 1)[0] not in backend_modules:
                    continue
                line = text.count("\n", 0, match.start()) + 1
                record = f"{relative}:{line}: import {imported}"
                (tests if relative.startswith("source/tests/") else engine).append(record)
    return {"engine": engine, "tests": tests, "modules": sorted(backend_modules), "scanned": scanned}


def write_json(path: str, data: dict) -> None:
    """先写临时文件再替换，失败不能留下半份基线。"""
    parent = os.path.dirname(os.path.abspath(path))
    os.makedirs(parent, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=parent,
                                         suffix=".json.tmp", delete=False) as handle:
            temporary = handle.name
            json.dump(data, handle, indent=2, sort_keys=True)
            handle.write("\n")
        os.replace(temporary, path)
    finally:
        if temporary is not None and os.path.exists(temporary):
            os.unlink(temporary)


def main() -> int:
    repo_root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    scripts_dir = os.path.dirname(os.path.abspath(__file__))
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--config", choices=sorted(CONFIGURATIONS), default="dynamic",
                        help="WHICH runtime configuration this measures: since S5 there is ONE (the dynamic "
                             "runtime's C-entry boundary), and it picks the baseline+whitelist pair and the "
                             "default build dir (build-release-dyn-clang64). Printed on every run, --quiet "
                             "included.")
    parser.add_argument("--build-dir", default=None,
                        help="the build tree holding both archives (default: per --config)")
    parser.add_argument("--baseline", default=None,
                        help="baseline json (default: per-toolchain, see the module docstring)")
    parser.add_argument("--update", action="store_true", help="shrink the baseline to today's symbol set")
    parser.add_argument("--list", action="store_true", help="print the worklist, demangled")
    parser.add_argument("--warn", action="store_true",
                        help="make ordinary checks nonblocking; baseline mutations and flip gate stay strict")
    parser.add_argument("--quiet", action="store_true", help="only report the verdict")
    parser.add_argument("--initialize", action="store_true", help="create a missing baseline, never overwrite one")
    parser.add_argument("--consumer", action="append", default=[], help="additional engine/application object or archive")
    parser.add_argument("--app-object", action="append", default=[],
                        help="explicit principal application object (for nonstandard build layouts)")
    parser.add_argument("--report", help="write machine-readable measurement and set delta")
    parser.add_argument("--require-zero", action="store_true",
                        help="enforce the flip gate: nothing measured outside the whitelist, no stale whitelist "
                             "entry, no whitelist entry the ratchet never tracked, application evidence, and no "
                             "engine/application import of a module deren_vulkan owns")
    parser.add_argument("--whitelist", default=None,
                        help="the flip gate's whitelist (default: source/scripts/backend_boundary_whitelist.json; "
                             "shrink-only, hand-edited, never written by --update)")
    parser.add_argument("--repo-root", default=None,
                        help="the tree whose CMakeLists names deren_vulkan's modules and whose sources are "
                             "scanned for engine/application imports of them (default: the repository root)")
    args = parser.parse_args()

    # ---- WHICH CONFIGURATION IS BEING MEASURED, PRINTED BEFORE ANYTHING ELSE ---------------------
    # A blind `--quiet` run once read as a dynamic-tree measurement while it was measuring the legacy
    # pair again. The identification is therefore deliberately NOT part of the quiet-able report.
    configuration = CONFIGURATIONS[args.config]
    print(f"config   {args.config}: {configuration['description']}")

    # `--config` picks the default build tree; it must be resolved BEFORE anything joins paths with it
    # (the crash this fixes: the `None` default reached ntpath.join in flavor_of()). The default is made
    # ABSOLUTE against the repository root, as it was before `--config` existed, so the tool still works
    # from any working directory.
    if args.build_dir is None:
        args.build_dir = configuration["build_dir"]
        if not os.path.isabs(args.build_dir):
            args.build_dir = os.path.join(repo_root, args.build_dir)
    flavor = flavor_of(args.build_dir)
    if args.baseline is None:
        args.baseline = os.path.join(scripts_dir, configuration["baseline"].format(flavor=flavor))
    if args.whitelist is None:
        args.whitelist = os.path.join(scripts_dir, configuration["whitelist"])
    if args.repo_root is None:
        args.repo_root = repo_root
    print(f"tree     {args.build_dir}  (flavor {flavor})")
    print(f"baseline {args.baseline}")
    print(f"whitelist {args.whitelist}")

    nm = find_tool(NM_CANDIDATES)
    if not nm:
        raise SystemExit("check_backend_boundary: no nm found (looked for "
                         + ", ".join(NM_CANDIDATES) + ")")
    filt = find_tool(FILT_CANDIDATES)

    # THE BACKEND'S ARTIFACT IS A DLL SINCE THE FLIP, so this lookup is allowed to come up empty: the
    # export table below is the surface then, and "no archive" is the CORRECT state rather than a
    # missing build. A static tree still resolves an archive and is measured the old way.
    backend_dll = next((os.path.join(args.build_dir, name) for name in BACKEND_DLL_NAMES
                        if os.path.isfile(os.path.join(args.build_dir, name))), None)
    backend_archive = None if backend_dll is not None else resolve_archive(args.build_dir, BACKEND_TARGET)
    kit_archive = resolve_archive(args.build_dir, KIT_TARGET)

    # A HALF-BUILT TREE UNDER-REPORTS, AND IT DID: rebuilding `deren_vulkan` alone leaves
    # `deren_engine.a` referencing symbols the backend no longer defines, so those references drop
    # out of the intersection and the count FALLS without a line of engine code having changed
    # (measured: 78 -> 76 that way, against 78 -> 77 for the change that was actually made). The
    # Consumers more than 5 minutes older than the backend fail; the build target completes
    # the product first. This timestamp guard is conservative, not proof of a clean build.
    # THE BACKEND'S PUBLIC SURFACE, in whichever shape this tree has: a DLL's EXPORT TABLE since the
    # SHARED flip, an archive's symbol table in a static tree. The names are the same kind of thing
    # (what a consumer could resolve), which is why the rest of the measurement does not care.
    if backend_dll is not None:
        exports = read_backend_exports(backend_dll) or []
        defined = {name: [os.path.basename(backend_dll)] for name in exports}
        backend_undefined, _ = {}, set()
    else:
        defined, _ = read_symbols(backend_archive, nm, defined=True)
        backend_undefined, _ = read_symbols(backend_archive, nm, defined=False)
    engine_defined, _ = read_symbols(kit_archive, nm, defined=True)
    undefined, kit_members = read_symbols(kit_archive, nm, defined=False)

    # 不只量引擎归档：main/chores 也可能直接引入后端依赖。
    from pathlib import Path
    build_path = Path(args.build_dir)
    application = []
    chores = []
    for name in ("libchores.a", "chores.lib"):
        if (build_path / name).is_file():
            chores.append(str(build_path / name))
    application.extend(chores)
    main_dir = build_path / "CMakeFiles" / "deren.dir"
    main_objects = []
    if main_dir.is_dir():
        main_objects = [str(p) for p in main_dir.rglob("main.cpp.*") if p.suffix in (".obj", ".o")]
    application.extend(main_objects)
    application.extend(args.consumer)
    explicit_app = [p for p in args.app_object if not same_artifact(p, kit_archive)]
    application.extend(explicit_app)
    application = list(dict.fromkeys(os.path.abspath(p) for p in application
                                    if not same_artifact(p, kit_archive)))
    consumers = [{"file": os.path.abspath(kit_archive), "members": sorted(kit_members)}]
    stale = []
    for path in [kit_archive, *application]:
        if backend_archive is not None and same_artifact(path, backend_archive):
            print(f"FAIL: backend archive cannot serve as an engine/application consumer: {path}")
            return 1
        if not os.path.isfile(path):
            print(f"FAIL: consumer does not exist: {path}")
            return 1
        if backend_archive is not None and os.path.getmtime(path) < os.path.getmtime(backend_archive) - 300.0:
            stale.append(path)
        if os.path.abspath(path) == os.path.abspath(kit_archive):
            continue
        consumer_defined, _ = read_symbols(path, nm, defined=True)
        consumer_undefined, members = read_symbols(path, nm, defined=False)
        engine_defined.update(consumer_defined)
        consumers.append({"file": path, "members": sorted(members)})
        for symbol, sites in consumer_undefined.items():
            undefined[symbol].extend(f"{os.path.basename(path)}:{site}" for site in sites)

    # 后端自己能定义的符号不是反向依赖；归档的不同成员会同时报告 D 和 U。
    reverse = sorted((set(backend_undefined) - set(defined)) & set(engine_defined))
    application_evidence = bool(explicit_app) or bool(main_objects and chores)

    cross = {s: undefined[s] for s in undefined if s in defined}
    usages = sum(len(undefined[s]) for s in cross)

    symbols = sorted(cross)
    owning = [s for s in symbols if carries_owning_stl(s)]
    report = {
        "backend_archive": os.path.basename(backend_archive or backend_dll),
        "defined_in_backend": len(defined),
        "kit_archive": os.path.basename(kit_archive),
        "kit_members": len(kit_members),
        "cross_boundary_symbols": symbols,
        "count": len(symbols),
        "usages": usages,
        "owning_stl_symbols": owning,
        "owning_stl_count": len(owning),
        "consumers": consumers,
        "reverse_boundary_symbols": reverse,
        "stale_consumers": stale,
        "nm": os.path.abspath(nm),
        "symbol_sources": cross,
        "application_evidence": {"complete": application_evidence, "main_objects": main_objects,
                                 "chores_archives": chores, "explicit_app_objects": explicit_app},
    }

    try:
        baseline = load_baseline(args.baseline)
    except ValueError as error:
        print(f"FAIL: {error}")
        return 1
    report["joiners"] = sorted(set(symbols) - set(baseline["cross_boundary_symbols"])) if baseline else symbols
    report["leavers"] = sorted(set(baseline["cross_boundary_symbols"]) - set(symbols)) if baseline else []

    # ---- THE FLIP GATE'S OTHER TWO INSTRUMENTS -------------------------------------------------
    # (1) the whitelist: which of today's symbols the flip is still allowed to carry, and why;
    # (2) the IMPORT GRAPH: a symbol set can reach zero while the engine keeps importing the backend's
    #     modules - and a module import is what makes a SHARED backend impossible, because the BMI
    #     would have to come from the DLL. The symbol count cannot see that layer; this can.
    try:
        whitelist = load_whitelist(args.whitelist)
    except ValueError as error:
        print(f"FAIL: {error}")
        return 1
    whitelist_symbols = set(whitelist["symbols"]) if whitelist else set()
    whitelist_hits = sorted(whitelist_symbols & set(symbols))
    whitelist_stale = sorted(whitelist_symbols - set(symbols))
    baseline_symbols = set(baseline["cross_boundary_symbols"]) if baseline else set()
    whitelist_untracked = sorted(whitelist_symbols - baseline_symbols)
    backend_modules, backend_files = backend_modules_of(args.repo_root)
    imports = scan_backend_imports(args.repo_root, backend_modules, backend_files)
    report["whitelist"] = {"path": whitelist["path"] if whitelist else None,
                           "count": len(whitelist_symbols), "hits": whitelist_hits, "stale": whitelist_stale,
                           "untracked_by_the_ratchet": whitelist_untracked}
    report["backend_modules"] = sorted(backend_modules)
    report["backend_imports"] = {"engine_application": imports["engine"], "tests_informational": imports["tests"],
                                 "sources_scanned": imports["scanned"]}
    report["cpp_runtime"] = check_cpp_runtime(args.build_dir)
    report["shared_utility"] = check_shared_utility(args.build_dir, nm)
    report["dll_surface"] = check_dll_surface(args.build_dir)
    report["backend_surface_shape"] = "export table" if backend_dll is not None else "archive symbol table"

    if args.report:
        if os.path.normcase(os.path.realpath(args.report)) == os.path.normcase(os.path.realpath(args.baseline)):
            print("FAIL: report path must differ from baseline path")
            return 1
        write_json(args.report, report)

    failed = False
    if stale:
        print("FAIL: consumers are over 5 minutes older than the backend; rebuild all targets before measuring")
        for path in stale:
            print(f"    STALE  {path}")
        failed = True
    if reverse:
        print("FAIL: the backend depends on engine/application symbols")
        for symbol in reverse:
            print(f"    REVERSE  {symbol}")
        failed = True
    if args.require_zero:
        if whitelist is None:
            print(f"FAIL: the flip gate needs its whitelist at {args.whitelist} (the file is missing)")
            failed = True
        outside = sorted(set(symbols) - whitelist_symbols)
        if outside:
            print(f"FAIL: {len(outside)} measured symbol(s) are OUTSIDE the whitelist "
                  f"({len(symbols)} measured, {len(whitelist_symbols)} whitelisted)")
            for symbol in outside:
                print(f"    OUTSIDE  {symbol}")
            failed = True
        if whitelist_stale:
            print(f"FAIL: {len(whitelist_stale)} whitelist entr(ies) have NO live hit - the whitelist may only "
                  f"shrink, so delete them")
            for symbol in whitelist_stale:
                print(f"    STALE    {symbol}")
            failed = True
        if whitelist_untracked:
            print(f"FAIL: {len(whitelist_untracked)} whitelist entr(ies) name symbols the ratchet never tracked - "
                  f"a new dependency cannot be whitelisted into existence")
            for symbol in whitelist_untracked:
                print(f"    UNTRACKED {symbol}")
            failed = True
        if not application_evidence:
            print("FAIL: the flip gate needs main/chores evidence (or explicit --app-object for another layout)")
            failed = True
        # ---- (b) THE C++ RUNTIME PREMISE -------------------------------------------------------
        # The utility split's STL-across-the-seam ruling is only sound while one libc++ (and one heap)
        # is shared by every image. A STATIC libc++ makes the DLL carry a private runtime and heap, and
        # then an allocation on one side and a free on the other is undefined - so a tree that linked
        # it statically must not read as green. The check reads whichever images exist; the
        # application-evidence rule above is what refuses a tree that has none.
        cpp_runtime = report["cpp_runtime"]
        for image_name, entry in cpp_runtime["images"].items():
            if not entry["dynamic_cxx_runtime"]:
                print(f"FAIL: {image_name} does not import {cpp_runtime['dll']} - it was linked against a "
                      f"STATIC C++ runtime, so it holds a private runtime and a private heap. The utility "
                      f"split's STL-across-the-seam ruling (batch ④) depends on ONE shared runtime; "
                      f"relinking with the default dynamic runtime is what fixes this.")
                failed = True
        # ---- (a) ONE COPY OF THE PROCESS-WIDE HALF -----------------------------------------------
        # The split exists because the log sink must exist once per process. Both halves are STATIC in
        # this batch, so the archive layering is the evidence: the shared TUs live in exactly one
        # archive, and the engine and the backend reference what it defines.
        shared_utility = report["shared_utility"]
        if shared_utility["copies"]:
            print(f"FAIL: {len(shared_utility['copies'])} archive(s) carry their OWN copy of the "
                  f"process-wide utility TUs - the log sink would exist twice in one process")
            for copy in shared_utility["copies"]:
                print(f"    COPY     {copy['archive']}: {', '.join(copy['members'])}")
            failed = True
        for archive_name, references in shared_utility["referenced_by"].items():
            if references == 0:
                print(f"FAIL: {archive_name} references NONE of {UTILITY_SHARED_ARCHIVE}'s defined symbols - "
                      f"it either does not use the shared toolkit or carries its own copy of it")
                failed = True
        if imports["engine"]:
            engine_files = sorted({record.split(":", 1)[0] for record in imports["engine"]})
            print(f"FAIL: {len(imports['engine'])} import site(s) in {len(engine_files)} engine/application file(s) "
                  f"still import a module deren_vulkan owns - the backend cannot be a DLL while that is true")
            for record in imports["engine"]:
                print(f"    IMPORT   {record}")
            failed = True
    if args.initialize:
        if baseline is not None or args.update:
            print("FAIL: --initialize requires a missing baseline and cannot be combined with --update")
            return 1
        if failed:
            return 1
        write_json(args.baseline, report)
        print(f"check_backend_boundary: initialized {len(symbols)} symbols -> {args.baseline}")
        return 0

    if not args.quiet:
        print(f"backend  {os.path.basename(backend_archive or backend_dll):<24} "
              f"{len(defined)} defined symbols")
        print(f"engine   {os.path.basename(kit_archive):<24} "
              f"{len(kit_members)} members; {len(cross)} unique backend symbols across {len(consumers)} consumer(s)")
        print(f"usage    {usages} reference sites; {len(owning)} symbol(s) carry owning STL")
        # (b) the C++ runtime premise: printed every run, because it is what makes the utility
        # split's relaxed export rules sound (see the helper's note).
        cpp_runtime = report["cpp_runtime"]
        if cpp_runtime["images"]:
            for image_name, entry in cpp_runtime["images"].items():
                state = f"imports {cpp_runtime['dll']}" if entry["dynamic_cxx_runtime"] else f"MISSING {cpp_runtime['dll']} (STATIC C++ runtime)"
                print(f"cxx      {image_name:<24} {state}")
        else:
            print(f"cxx      no deren.exe/deren_vulkan.dll in this tree - nothing to read (see the flip gate's "
                  f"application evidence)")
        # ---- THE DLL SURFACE (the flip's own instrument) ---------------------------------------
        # Three questions, all on the artifact: the export table carries exactly the one designed
        # entry, the DLL imports the process-wide half (one log sink per process), and the package's
        # second half is really there.
        dll_surface = report["dll_surface"]
        if dll_surface["dll"] is not None:
            if dll_surface["exports"] != ["deren_make_api_core"]:
                print(f"FAIL: {dll_surface['dll']} exports {len(dll_surface['exports'])} name(s); the boundary is "
                      f"exactly one ('deren_make_api_core'). Everything else that is exported became part of the "
                      f"ABI by accident: {', '.join(dll_surface['exports']) or '(nothing)'}")
                failed = True
            if not dll_surface["imports_shared_utility"]:
                print(f"FAIL: {dll_surface['dll']} does not import {SHARED_UTILITY_DLL} - the process-wide half is "
                      f"missing from the package, so the backend would carry its OWN copy of the log sink")
                failed = True
        elif application_evidence and backend_archive is None:
            # A DLL tree must HAVE its DLL (the loader resolves it by name at run time); a static tree
            # has an archive instead and is measured through it, which is why this is not an error then.
            print(f"FAIL: application evidence is present but no backend DLL was found in {args.build_dir} - "
                  f"looked for {', '.join(BACKEND_DLL_NAMES)}")
            failed = True
        # (a) one copy of the process-wide half, from the archives
        shared_utility = report["shared_utility"]
        if shared_utility["defined_symbols"]:
            references = ", ".join(f"{name}={count}" for name, count in shared_utility["referenced_by"].items())
            print(f"shared   {(shared_utility['dll'] or shared_utility['archive']):<24} {shared_utility['defined_symbols']} defined symbols; "
                  f"references to them: {references}; own copies elsewhere: {len(shared_utility['copies'])}")
        else:
            print("shared   no shared_utility.dll / libshared_utility.a in this tree - nothing to read")
        # the DLL surface, printed every run
        dll_surface = report["dll_surface"]
        if dll_surface["dll"] is not None:
            print(f"dll      {dll_surface['dll']:<24} exports: {', '.join(dll_surface['exports']) or '(none)'}; "
                  f"imports {SHARED_UTILITY_DLL}: {'yes' if dll_surface['imports_shared_utility'] else 'NO'}")
        else:
            print("dll      no backend DLL in this tree - the backend is still an archive")
        print()

        by_category: dict[str, int] = defaultdict(int)
        for symbol in symbols:
            by_category[category(symbol)] += 1
        print("by category (symbols):")
        for name, count in sorted(by_category.items(), key=lambda kv: -kv[1]):
            print(f"    {count:>4}  {name}")

        by_area: dict[str, int] = defaultdict(int)
        for symbol in symbols:
            seen = set()
            for member in cross[symbol]:
                area = area_of(member)
                if (area, member) not in seen:
                    by_area[area] += 1
                    seen.add((area, member))
        print("by area (symbol/member pairs, one symbol can appear in several):")
        for name, count in sorted(by_area.items(), key=lambda kv: -kv[1]):
            print(f"    {count:>4}  {name}")
        print()

        # THE WHITELIST, HIT BY HIT (the flip gate's exception list, never a silent allow-list)
        if whitelist is None:
            print(f"whitelist (missing): {args.whitelist} - --require-zero will refuse to run without it")
        else:
            print(f"whitelist {whitelist['path']}")
            print(f"    {len(whitelist_hits)} hit, {len(whitelist_stale)} stale, {len(whitelist_untracked)} untracked")
            for entry in whitelist["entries"]:
                marker = "HIT    " if entry["symbol"] in set(whitelist_hits) else "STALE  "
                print(f"    {marker}{entry['symbol']}")
                print(f"           {entry['reason']}")
            print()

        # THE IMPORT GRAPH (the layer the symbol count cannot see)
        engine_files = {record.split(":", 1)[0] for record in imports["engine"]}
        print(f"imports  engine/application: {len(imports['engine'])} site(s) in {len(engine_files)} file(s) import a "
              f"module deren_vulkan owns (of {imports['scanned']} source(s) scanned; {len(backend_modules)} backend module(s))")
        for record in imports["engine"][:10]:
            print(f"    IMPORT   {record}")
        if len(imports["engine"]) > 10:
            print(f"    ... and {len(imports['engine']) - 10} more (--list prints all)")
        print(f"imports  tests (informational, not part of the flip gate): {len(imports['tests'])} file(s)")
        print()

    if args.list:
        print("worklist:")
        for symbol in symbols:
            shown = symbol
            if filt:
                proc = subprocess.run([filt, symbol], capture_output=True, text=True)
                if proc.returncode == 0 and proc.stdout.strip():
                    shown = proc.stdout.strip()
            marker = "  [owning STL]" if carries_owning_stl(symbol) else ""
            print(f"    {shown}{marker}")
            print(f"        <- {', '.join(sorted(set(cross[symbol])))}")
        print()
        print("backend imports still reached from the engine/application (the flip's real blocker):")
        for record in imports["engine"]:
            print(f"    {record}")
        if not imports["engine"]:
            print("    (none)")
        print()
        print("backend modules deren_vulkan owns (from CMake's target_sources):")
        for module in sorted(backend_modules):
            print(f"    {module}")
        print()

    if baseline is None:
        print(f"check_backend_boundary: no baseline at {args.baseline}; "
              f"run --initialize to explicitly record today's {len(symbols)}")
        return 0 if args.warn and not args.update and not args.require_zero else 1

    allowed = int(baseline.get("count", 0))
    allowed_owning = int(baseline.get("owning_stl_count", 0))
    added = report["joiners"]
    if added:
        print(f"FAIL: {len(added)} NEW backend dependencies ({len(symbols)} symbols, baseline {allowed})")
        for symbol in added[:20]:
            print(f"    NEW  {symbol}")
        if len(added) > 20:
            print(f"    ... and {len(added) - 20} more")
        failed = True

    if len(owning) > allowed_owning:
        print(f"FAIL: {len(owning)} symbols carry an owning STL type across the boundary, "
              f"baseline is {allowed_owning} (plan §4.2: no STL across the boundary)")
        for symbol in owning:
            print(f"    OWNING  {symbol}")
        failed = True

    if failed:
        return 0 if args.warn and not args.update and not args.require_zero else 1

    if args.update:
        write_json(args.baseline, report)
        print(f"check_backend_boundary: baseline ratcheted down to {len(symbols)} symbols "
              f"({usages} sites, {len(owning)} with owning STL) -> {args.baseline}")
        return 0

    if len(symbols) < allowed:
        print(f"OK (improved): {len(symbols)} symbols, baseline still {allowed} - "
              f"run --update to ratchet the baseline down")
        return 0

    print(f"OK: {len(symbols)} symbols, baseline {allowed}, {usages} reference sites, "
          f"{len(owning)} with owning STL")
    if args.require_zero:
        print(f"flip gate: every measured symbol is whitelisted ({len(whitelist_hits)} hit), the whitelist has no "
              f"stale entry, and no engine/application source imports a deren_vulkan module")
        print("DLL import/export gates remain separate")
    elif allowed == 0:
        print("archive boundary is zero; run --require-zero with application consumers before the flip")
    return 0


if __name__ == "__main__":
    sys.exit(main())
