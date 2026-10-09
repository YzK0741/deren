"""Native census must distinguish engine sources from graphics plugins."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / "source/scripts/check_native_boundary.py"
spec = importlib.util.spec_from_file_location("native_boundary", SCRIPT)
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


class NativeScopeTests(unittest.TestCase):
    def test_system_vulkan_include_alone_is_forbidden(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / "source/engine/header_only.cppm"
            path.parent.mkdir(parents=True)
            path.write_text("#include <vulkan/vulkan.h>\n", encoding="utf-8")
            (root / "CMakeLists.txt").write_text("", encoding="utf-8")
            previous = gate.REPO
            try:
                gate.REPO = root
                self.assertEqual(["source/engine/header_only.cppm: 1 -> entry point:#include <vulkan/"],
                                 gate.check_tokens(False))
            finally:
                gate.REPO = previous

    def test_source_engine_filters_and_layout_are_scanned(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            paths = ["source/engine/filters/filters.cppm",
                     "source/engine/render_layout/render_layout.cppm"]
            for relative in paths:
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("VkDevice forbidden;", encoding="utf-8")
            (root / "CMakeLists.txt").write_text("", encoding="utf-8")
            previous = gate.REPO
            try:
                gate.REPO = root
                self.assertEqual(paths, [path.relative_to(root).as_posix()
                                         for path in gate.engine_sources()])
                self.assertEqual(2, len(gate.check_tokens(False)))
            finally:
                gate.REPO = previous

    def test_gui_plugin_is_excluded_but_engine_violation_remains(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sources = {
                "source/backends/vulkan/core/backend.cpp": "VkDevice backend;",
                "source/backends/vulkan/graphical_user_interface/gui.cppm": "VkDevice gui;",
                "source/backends/vulkan/graphical_user_interface/gui_dll.cpp": "VkQueue gui_queue;",
                "source/engine/runtime/engine.cpp": "VkDevice engine;",
            }
            for relative, code in sources.items():
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(code, encoding="utf-8")
            (root / "CMakeLists.txt").write_text(
                "target_sources(deren_vulkan\n PRIVATE\n source/backends/vulkan/core/backend.cpp\n)\n"
                "target_sources(deren_gui_vulkan\n PUBLIC FILE_SET CXX_MODULES FILES\n"
                " source/backends/vulkan/graphical_user_interface/gui.cppm\n PRIVATE\n"
                " source/backends/vulkan/graphical_user_interface/gui_dll.cpp\n)\n", encoding="utf-8")
            previous = gate.REPO
            try:
                gate.REPO = root
                self.assertEqual(["source/engine/runtime/engine.cpp"],
                                 [path.relative_to(root).as_posix() for path in gate.engine_sources()])
                self.assertEqual(["source/engine/runtime/engine.cpp: 1 -> type:VkDevice"], gate.check_tokens(False))
            finally:
                gate.REPO = previous


if __name__ == "__main__":
    unittest.main()
