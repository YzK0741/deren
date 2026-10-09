"""Regression tests of the boundary CLI using real compiler-produced archives."""
from pathlib import Path
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "scripts/check_backend_boundary.py"


class BoundaryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiler = shutil.which("gcc") or shutil.which("clang")
        cls.archiver = shutil.which("ar") or shutil.which("llvm-ar")
        if not all((cls.compiler, cls.archiver, shutil.which("nm") or shutil.which("llvm-nm"))):
            raise RuntimeError("Boundary tests need a C compiler, ar, and nm")

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.baseline = self.root / "baseline.json"
        self.whitelist = self.root / "whitelist.json"
        self.make_archive("libderen_vulkan.a", "int backend_a(void) { return 1; } int backend_b(void) { return 2; }")
        self.make_archive("libderen_engine.a", "extern int backend_a(void); int engine_entry(void) { return backend_a(); }")
        self.write_baseline(["backend_a"])
        self.write_whitelist([])
        self.write_synthetic_repo()

    def make_archive(self, name, source):
        path = self.root / (name + ".c")
        obj = self.root / (name + ".obj")
        path.write_text(source, encoding="utf-8")
        subprocess.run([self.compiler, "-c", str(path), "-o", str(obj)], check=True, capture_output=True)
        subprocess.run([self.archiver, "rcs", str(self.root / name), str(obj)], check=True, capture_output=True)
        return self.root / name

    def write_baseline(self, symbols):
        self.baseline.write_text(json.dumps({"count": len(symbols), "cross_boundary_symbols": symbols,
                                             "owning_stl_count": 0, "owning_stl_symbols": []}), encoding="utf-8")

    def write_whitelist(self, entries):
        """(symbol, reason) pairs; the flip gate's shrink-only exception list."""
        payload = {"count": len(entries), "entries": [{"symbol": symbol, "reason": reason} for symbol, reason in entries]}
        self.whitelist.write_text(json.dumps(payload), encoding="utf-8")

    def write_synthetic_repo(self, engine_imports=()):
        """A tree the import check can read: CMake names deren_vulkan's module file, that file declares
        the module, and every name in `engine_imports` becomes an engine source importing it."""
        core = self.root / "source" / "backends" / "vulkan" / "core"
        core.mkdir(parents=True, exist_ok=True)
        (self.root / "CMakeLists.txt").write_text(
            "add_library(deren_vulkan STATIC)\n"
            "target_sources(deren_vulkan\n"
            "    PUBLIC\n"
            "        FILE_SET CXX_MODULES\n"
            "        FILES\n"
            "        source/backends/vulkan/core/core.cppm\n"
            ")\n", encoding="utf-8")
        (core / "core.cppm").write_text("export module deren.vulkan.core;\n", encoding="utf-8")
        for relative in engine_imports:
            path = self.root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("import deren.vulkan.core;\n", encoding="utf-8")

    def run_gate(self, *args):
        return subprocess.run([sys.executable, str(SCRIPT), "--build-dir", str(self.root),
                               "--baseline", str(self.baseline), "--whitelist", str(self.whitelist),
                               "--repo-root", str(self.root), *args], capture_output=True, text=True)

    def run_with_config_default(self, *args):
        """Run WITHOUT the explicit pair/build-dir, so `--config`'s own defaults are what is exercised.
        The archives are placed in a directory named after neither real build tree, so the path the gate
        prints is the only thing under test here."""
        return subprocess.run([sys.executable, str(SCRIPT), *args], capture_output=True, text=True)

    def make_application(self):
        return self.make_archive("application.a", "int app_entry(void) { return 0; }")

    def test_existing_dependency_passes(self):
        self.assertEqual(self.run_gate().returncode, 0)

    def test_same_count_new_dependency_fails(self):
        self.make_archive("libderen_engine.a", "extern int backend_b(void); int engine_entry(void) { return backend_b(); }")
        result = self.run_gate()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("backend_b", result.stdout)

    def test_reduction_does_not_hide_new_dependency(self):
        self.write_baseline(["backend_a", "retired_dependency"])
        self.make_archive("libderen_engine.a", "extern int backend_b(void); int engine_entry(void) { return backend_b(); }")
        self.assertEqual(self.run_gate().returncode, 1)

    def test_growing_update_preserves_baseline_bytes(self):
        self.write_baseline([])
        before = self.baseline.read_bytes()
        result = self.run_gate("--update")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual(self.baseline.read_bytes(), before)

    def test_replacement_update_is_rejected(self):
        self.make_archive("libderen_engine.a", "extern int backend_b(void); int engine_entry(void) { return backend_b(); }")
        before = self.baseline.read_bytes()
        self.assertEqual(self.run_gate("--update").returncode, 1)
        self.assertEqual(self.baseline.read_bytes(), before)

    def test_warn_never_accepts_growing_update(self):
        self.write_baseline([])
        before = self.baseline.read_bytes()
        self.assertEqual(self.run_gate("--warn", "--update").returncode, 1)
        self.assertEqual(self.baseline.read_bytes(), before)

    def test_valid_reduction_can_update(self):
        self.write_baseline(["backend_a", "retired_dependency"])
        self.assertEqual(self.run_gate("--update").returncode, 0)
        self.assertEqual(json.loads(self.baseline.read_text())["cross_boundary_symbols"], ["backend_a"])

    def test_missing_baseline_is_failure(self):
        self.baseline.unlink()
        self.assertEqual(self.run_gate().returncode, 1)

    def test_missing_baseline_requires_explicit_initialization(self):
        self.baseline.unlink()
        self.assertEqual(self.run_gate("--update").returncode, 1)
        self.assertFalse(self.baseline.exists())

    def test_initialization_records_first_set(self):
        self.baseline.unlink()
        result = self.run_gate("--initialize")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(json.loads(self.baseline.read_text())["cross_boundary_symbols"], ["backend_a"])

    def test_initialization_cannot_overwrite_existing_baseline(self):
        before = self.baseline.read_bytes()
        self.assertEqual(self.run_gate("--initialize").returncode, 1)
        self.assertEqual(self.baseline.read_bytes(), before)

    def test_reverse_dependency_is_failure(self):
        self.make_archive("libderen_vulkan.a", "extern int engine_entry(void); int backend_a(void) { return engine_entry(); }")
        result = self.run_gate()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("engine_entry", result.stdout)

    def test_stale_archive_blocks_update(self):
        self.write_baseline(["backend_a", "retired_dependency"])
        backend_time = (self.root / "libderen_vulkan.a").stat().st_mtime
        os.utime(self.root / "libderen_engine.a", (backend_time - 600, backend_time - 600))
        before = self.baseline.read_bytes()
        self.assertEqual(self.run_gate("--update").returncode, 1)
        self.assertEqual(self.baseline.read_bytes(), before)

    def test_bad_baseline_is_named_failure(self):
        self.baseline.write_text('{"count":0,"cross_boundary_symbols":["backend_a"]}', encoding="utf-8")
        result = self.run_gate()
        self.assertEqual(result.returncode, 1)
        self.assertIn("baseline", result.stdout + result.stderr)
        self.assertNotIn("Traceback", result.stderr)

    def test_report_exposes_members_and_set_delta(self):
        self.write_baseline(["backend_a", "retired_dependency"])
        report = self.root / "report.json"
        result = self.run_gate("--report", str(report))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        data = json.loads(report.read_text())
        self.assertEqual(data["joiners"], [])
        self.assertEqual(data["leavers"], ["retired_dependency"])
        self.assertEqual(data["reverse_boundary_symbols"], [])
        self.assertEqual(data["count"], 1)
        self.assertEqual(data["usages"], 1)
        self.assertEqual(len(data["consumers"][0]["members"]), 1)

    def test_application_cannot_bypass_archive_measurement(self):
        consumer = self.make_archive("application.a", "extern int backend_b(void); int app_entry(void) { return backend_b(); }")
        result = self.run_gate("--consumer", str(consumer))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("backend_b", result.stdout)

    def test_nonzero_ratchet_does_not_pass_flip_gate(self):
        result = self.run_gate("--require-zero", "--consumer", str(self.root / "libderen_engine.a"))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)

    # ---- the flip gate's two instruments (whitelist + import graph) ----------------------------------
    def test_flip_gate_rejects_a_symbol_outside_the_whitelist(self):
        app = self.make_application()
        result = self.run_gate("--require-zero", "--app-object", str(app))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("OUTSIDE", result.stdout)
        self.assertIn("backend_a", result.stdout)

    def test_flip_gate_accepts_a_whitelisted_symbol_and_prints_the_hit(self):
        self.write_whitelist([("backend_a", "fixture: the one dependency this test whitelists")])
        app = self.make_application()
        result = self.run_gate("--require-zero", "--app-object", str(app))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("HIT", result.stdout)
        self.assertIn("fixture: the one dependency this test whitelists", result.stdout)

    def test_flip_gate_rejects_a_whitelist_entry_with_no_hit(self):
        # the exception it claims no longer exists: the whitelist may only shrink, so it must be deleted
        self.write_whitelist([("backend_a", "live"), ("retired_dependency", "no longer measured")])
        app = self.make_application()
        result = self.run_gate("--require-zero", "--app-object", str(app))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("STALE", result.stdout)
        self.assertIn("retired_dependency", result.stdout)

    def test_flip_gate_rejects_a_whitelist_entry_the_ratchet_never_tracked(self):
        # a NEW dependency cannot be whitelisted into existence: the ratchet never recorded it
        self.make_archive("libderen_engine.a", "extern int backend_b(void); int engine_entry(void) { return backend_b(); }")
        self.write_whitelist([("backend_b", "added by hand to silence the gate")])
        app = self.make_application()
        result = self.run_gate("--require-zero", "--app-object", str(app))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("UNTRACKED", result.stdout)

    def test_flip_gate_rejects_a_missing_whitelist(self):
        self.whitelist.unlink()
        app = self.make_application()
        result = self.run_gate("--require-zero", "--app-object", str(app))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("whitelist", result.stdout)

    def test_flip_gate_rejects_an_engine_import_of_a_backend_module(self):
        self.write_synthetic_repo(engine_imports=["source/engine/runtime/runtime.cppm"])
        app = self.make_application()
        result = self.run_gate("--require-zero", "--app-object", str(app))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("import deren.vulkan.core", result.stdout)

    def test_import_reading_is_reported_without_blocking_the_ratchet(self):
        self.write_synthetic_repo(engine_imports=["source/engine/runtime/runtime.cppm"])
        result = self.run_gate()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("imports  engine/application: 1 site(s) in 1 file(s)", result.stdout)

    def test_an_engine_file_that_is_not_a_backend_module_is_not_flagged(self):
        # `deren.engine.filters` is declared by ENGINE code (deren_engine owns filters.cppm), so a
        # name prefix must not put it in the backend's set: only CMake's target list may.
        core = self.root / "source" / "engine" / "filters"
        core.mkdir(parents=True, exist_ok=True)
        (core / "filters.cppm").write_text("export module deren.engine.filters;\n", encoding="utf-8")
        (self.root / "source" / "engine" / "runtime" / "runtime.cppm").parent.mkdir(parents=True, exist_ok=True)
        (self.root / "source" / "engine" / "runtime" / "runtime.cppm").write_text("import deren.engine.filters;\n", encoding="utf-8")
        result = self.run_gate()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("imports  engine/application: 0 site(s)", result.stdout)

    def test_zero_gate_requires_application_evidence(self):
        self.make_archive("libderen_engine.a", "int engine_entry(void) { return 0; }")
        self.write_baseline([])
        result = self.run_gate("--require-zero")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)

    def test_zero_gate_accepts_clean_explicit_consumer(self):
        self.make_archive("libderen_engine.a", "int engine_entry(void) { return 0; }")
        self.write_baseline([])
        app = self.make_archive("application.a", "int app_entry(void) { return 0; }")
        result = self.run_gate("--require-zero", "--app-object", str(app))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_duplicate_engine_is_not_application_evidence(self):
        self.make_archive("libderen_engine.a", "int engine_entry(void) { return 0; }")
        self.write_baseline([])
        result = self.run_gate("--require-zero", "--consumer", str(self.root / "libderen_engine.a"))
        self.assertEqual(result.returncode, 1)

    def test_backend_archive_is_not_application_evidence(self):
        self.make_archive("libderen_engine.a", "int engine_entry(void) { return 0; }")
        self.write_baseline([])
        result = self.run_gate("--require-zero", "--app-object", str(self.root / "libderen_vulkan.a"))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("backend archive", result.stdout)

    def test_backend_hardlink_is_not_application_evidence(self):
        self.make_archive("libderen_engine.a", "int engine_entry(void) { return 0; }")
        self.write_baseline([])
        alias = self.root / "backend_alias.a"
        os.link(self.root / "libderen_vulkan.a", alias)
        result = self.run_gate("--require-zero", "--app-object", str(alias))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)

    def test_engine_hardlink_is_not_application_evidence(self):
        self.make_archive("libderen_engine.a", "int engine_entry(void) { return 0; }")
        self.write_baseline([])
        alias = self.root / "engine_alias.a"
        os.link(self.root / "libderen_engine.a", alias)
        result = self.run_gate("--require-zero", "--app-object", str(alias))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)

    def test_backend_cannot_be_added_as_engine_consumer(self):
        self.make_archive("libderen_engine.a", "int engine_entry(void) { return 0; }")
        self.write_baseline([])
        app = self.make_archive("application.a", "int app_entry(void) { return 0; }")
        result = self.run_gate("--require-zero", "--app-object", str(app),
                               "--consumer", str(self.root / "libderen_vulkan.a"))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("backend archive", result.stdout)

    @unittest.skipUnless(os.name == "nt", "Windows path case aliases")
    def test_case_alias_report_cannot_overwrite_baseline(self):
        before = self.baseline.read_bytes()
        result = self.run_gate("--report", str(self.baseline).upper())
        self.assertEqual(result.returncode, 1)
        self.assertEqual(self.baseline.read_bytes(), before)

    def test_chores_without_main_is_not_complete_application_evidence(self):
        self.make_archive("libderen_engine.a", "int engine_entry(void) { return 0; }")
        self.make_archive("libchores.a", "int chore_entry(void) { return 0; }")
        self.write_baseline([])
        self.assertEqual(self.run_gate("--require-zero").returncode, 1)

    def test_backend_internal_reference_is_not_a_reverse_dependency(self):
        self.make_archive("libderen_engine.a", "extern int backend_a(void); int shared_helper(void) { return 3; } int engine_entry(void) { return backend_a(); }")
        objects = []
        for name, source in (("backend_caller", "extern int shared_helper(void); int backend_a(void) { return shared_helper(); }"),
                             ("backend_provider", "int shared_helper(void) { return 5; }")):
            path = self.root / (name + ".c")
            obj = self.root / (name + ".obj")
            path.write_text(source, encoding="utf-8")
            subprocess.run([self.compiler, "-c", str(path), "-o", str(obj)], check=True, capture_output=True)
            objects.append(str(obj))
        (self.root / "libderen_vulkan.a").unlink()
        subprocess.run([self.archiver, "rcs", str(self.root / "libderen_vulkan.a"), *objects], check=True)
        result = self.run_gate()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    # ---- ONE CONFIGURATION, ONE PAIR (S5, the flip) ---------------------------------------------------
    #
    # The crash this guards against: the `--build-dir` default was `None` and reached `flavor_of()`'s
    # `os.path.join` before anything resolved it (TypeError in ntpath.join), so `--config` did not run at
    # all. `--config` now (1) resolves the default build dir FIRST, (2) picks the configuration's own
    # baseline+whitelist pair, and (3) PRINTS which configuration and which tree it measured, `--quiet`
    # included - a blind quiet run once read as a dynamic-tree measurement while measuring the legacy pair.
    #
    # S5 DELETED THE LEGACY RUNTIME (`source/engine/runtime/`) AND ITS PAIR WITH IT: one configuration remains,
    # and `--config legacy` is a NAMED refusal (argparse's choices) rather than a silent fallback to the
    # surviving pair - the failure mode the printed identification exists to prevent.
    def run_config_only(self, *args):
        return subprocess.run([sys.executable, str(SCRIPT), "--build-dir", str(self.root),
                               "--repo-root", str(self.root), *args], capture_output=True, text=True)

    def test_config_legacy_is_refused_after_s5(self):
        result = self.run_config_only("--config", "legacy")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("legacy", result.stderr)
        self.assertNotIn("config   legacy", result.stdout)

    def test_config_dynamic_picks_the_dynamic_pair(self):
        result = self.run_config_only("--config", "dynamic")
        self.assertNotIn("Traceback", result.stderr)
        self.assertIn("config   dynamic", result.stdout)
        self.assertIn("backend_boundary_baseline.dynamic.", result.stdout)
        self.assertIn("backend_boundary_whitelist_dynamic.json", result.stdout)
        self.assertIn(str(self.root), result.stdout)

    def test_config_identification_survives_quiet(self):
        result = self.run_config_only("--config", "dynamic", "--quiet")
        self.assertIn("config   dynamic", result.stdout)

    def test_unknown_config_is_refused(self):
        result = self.run_config_only("--config", "sideways")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("sideways", result.stderr)

    def test_one_configuration_remains_and_the_legacy_pair_is_gone(self):
        # S5 deleted the legacy runtime and its gate pair: the repository carries ONE baseline (this
        # toolchain's) and ONE whitelist. THE SHARED FLIP EMPTIED THAT LIST BY MEASUREMENT (abi 18 +
        # the by-name load): the boundary is ZERO, and every symbol that ever sat here is recorded as a
        # DEPARTURE rather than deleted silently - so the file still says what happened, and the DLL's
        # export table (exactly one `deren_*`) is the surface the flip gate checks instead.
        repository = Path(__file__).resolve().parents[1]
        scripts = repository / "scripts"
        self.assertFalse((scripts / "backend_boundary_whitelist.json").exists(),
                         "the legacy whitelist was deleted with source/backends/vulkan/runtime (S5)")
        self.assertFalse((scripts / "backend_boundary_baseline.mingw.json").exists(),
                         "the legacy baseline was deleted with source/backends/vulkan/runtime (S5)")
        dynamic_whitelist = scripts / "backend_boundary_whitelist_dynamic.json"
        self.assertTrue(dynamic_whitelist.is_file())
        payload = json.loads(dynamic_whitelist.read_text(encoding="utf-8"))
        self.assertEqual(payload["count"], len(payload["entries"]))
        self.assertEqual(payload["entries"], [], "the boundary reached zero in the SHARED flip")
        departures = {entry["symbol"] for entry in payload["departures"]}
        for symbol in ("deren_make_api_core", "deren_destroy_api_core", "deren_abi_version"):
            self.assertIn(symbol, departures, "a removed boundary symbol must be recorded as a departure")
        for entry in payload["departures"]:
            self.assertTrue(entry["reason"].strip())
            self.assertTrue(entry["left_in"].strip())


if __name__ == "__main__":
    unittest.main()
