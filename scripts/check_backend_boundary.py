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
    CMakeFiles/vulkancorekit.dir` still holds pre-split objects from before S1-A, and a
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
    in `scripts/backend_boundary_whitelist.json`: one entry per symbol, with the reason it is
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
    `target_sources(deren_vulkan ...)` list, NOT from a name prefix: `vulkan/core/filter/
    filters.cppm` declares `deren.vulkan.core.filters` but belongs to the ENGINE, while
    `vulkan/constant_init/constant_init.cppm` belongs to the BACKEND. Test sources are reported
    separately (they link the backend deliberately) and are not part of the gate.

THE SECOND NUMBER: OWNING STL ACROSS THE BOUNDARY
    A symbol whose signature carries `std::vector` / `std::string` / an allocator is an
    allocation that one image makes and the other frees (today:
    `init_utils::create_host_buffers(..., std::vector<vk_buffer>&, ...)`). The contract
    forbids it (plan_rhi_v4.md §4.2: no STL across the boundary), so the count is tracked
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

USAGE
    python scripts/check_backend_boundary.py                       # gate against the baseline
    python scripts/check_backend_boundary.py --update              # ratchet down to today
    python scripts/check_backend_boundary.py --list                # the worklist, demangled, + the imports
    python scripts/check_backend_boundary.py --warn                # ordinary checks report failures; mutations/flip stay strict
    python scripts/check_backend_boundary.py --require-zero        # the flip gate: whitelist-aware + import graph
    python scripts/check_backend_boundary.py --build-dir DIR
    python scripts/check_backend_boundary.py --whitelist PATH      # alternate whitelist (tests)
    python scripts/check_backend_boundary.py --repo-root DIR       # alternate tree for the import scan (tests)
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
KIT_TARGET = "vulkancorekit"

# Where the two halves land, per toolchain. MinGW/clang64 archives first, MSVC after.
ARCHIVE_NAMES = {
    BACKEND_TARGET: ("libderen_vulkan.a", "deren_vulkan.lib"),
    KIT_TARGET: ("libvulkancorekit.a", "vulkancorekit.lib"),
}

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


def flavor_of(build_dir: str) -> str:
    """Which baseline this tree's symbol sets are comparable with.

    The mangled names, and therefore the counts, differ per toolchain and per standard
    library, so the baseline is keyed rather than shared.
    """
    if any(os.path.isfile(os.path.join(build_dir, name)) for name in ARCHIVE_NAMES[BACKEND_TARGET][1:]):
        return "msvc"
    return "mingw" if sys.platform.startswith("win") else "posix"


MEMBER_RE = re.compile(r"^(?P<member>[^:\[\]]+\.(?:obj|o)):\s*$")


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

    OWNERSHIP COMES FROM THE TARGET, NOT FROM THE NAME. `vulkan/core/filter/filters.cppm` declares
    `deren.vulkan.core.filters` but belongs to the ENGINE target (vulkancorekit), while
    `vulkan/constant_init/constant_init.cppm` declares `deren.vulkan.constant_init` and belongs to the
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
                (tests if relative.startswith("tests/") else engine).append(record)
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
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    scripts_dir = os.path.dirname(os.path.abspath(__file__))
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", default=os.path.join(repo_root, "build-release-clang64"),
                        help="the build tree holding both archives")
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
                        help="the flip gate's whitelist (default: scripts/backend_boundary_whitelist.json; "
                             "shrink-only, hand-edited, never written by --update)")
    parser.add_argument("--repo-root", default=None,
                        help="the tree whose CMakeLists names deren_vulkan's modules and whose sources are "
                             "scanned for engine/application imports of them (default: the repository root)")
    args = parser.parse_args()

    if args.baseline is None:
        args.baseline = os.path.join(scripts_dir, f"backend_boundary_baseline.{flavor_of(args.build_dir)}.json")
    if args.whitelist is None:
        args.whitelist = os.path.join(scripts_dir, "backend_boundary_whitelist.json")
    if args.repo_root is None:
        args.repo_root = repo_root

    nm = find_tool(NM_CANDIDATES)
    if not nm:
        raise SystemExit("check_backend_boundary: no nm found (looked for "
                         + ", ".join(NM_CANDIDATES) + ")")
    filt = find_tool(FILT_CANDIDATES)

    backend_archive = resolve_archive(args.build_dir, BACKEND_TARGET)
    kit_archive = resolve_archive(args.build_dir, KIT_TARGET)

    # A HALF-BUILT TREE UNDER-REPORTS, AND IT DID: rebuilding `deren_vulkan` alone leaves
    # `vulkancorekit.a` referencing symbols the backend no longer defines, so those references drop
    # out of the intersection and the count FALLS without a line of engine code having changed
    # (measured: 78 -> 76 that way, against 78 -> 77 for the change that was actually made). The
    # Consumers more than 5 minutes older than the backend fail; the build target completes
    # the product first. This timestamp guard is conservative, not proof of a clean build.
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
        if same_artifact(path, backend_archive):
            print(f"FAIL: backend archive cannot serve as an engine/application consumer: {path}")
            return 1
        if not os.path.isfile(path):
            print(f"FAIL: consumer does not exist: {path}")
            return 1
        if os.path.getmtime(path) < os.path.getmtime(backend_archive) - 300.0:
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
        "backend_archive": os.path.basename(backend_archive),
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
        print(f"backend  {os.path.basename(backend_archive):<24} "
              f"{len(defined)} defined symbols")
        print(f"engine   {os.path.basename(kit_archive):<24} "
              f"{len(kit_members)} members; {len(cross)} unique backend symbols across {len(consumers)} consumer(s)")
        print(f"usage    {usages} reference sites; {len(owning)} symbol(s) carry owning STL")
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
