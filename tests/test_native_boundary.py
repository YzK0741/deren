"""Native census must distinguish engine sources from graphics plugins."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "scripts/check_native_boundary.py"
spec = importlib.util.spec_from_file_location("native_boundary", SCRIPT)
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


class NativeScopeTests(unittest.TestCase):
    def test_gui_plugin_is_excluded_but_engine_violation_remains(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sources = {
                "vulkan/core/backend.cpp": "VkDevice backend;",
                "vulkan/graphical_user_interface/gui.cppm": "VkDevice gui;",
                "vulkan/graphical_user_interface/gui_dll.cpp": "VkQueue gui_queue;",
                "runtime/engine.cpp": "VkDevice engine;",
            }
            for relative, code in sources.items():
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(code, encoding="utf-8")
            (root / "CMakeLists.txt").write_text(
                "target_sources(deren_vulkan\n PRIVATE\n vulkan/core/backend.cpp\n)\n"
                "target_sources(deren_gui_vulkan\n PUBLIC FILE_SET CXX_MODULES FILES\n"
                " vulkan/graphical_user_interface/gui.cppm\n PRIVATE\n"
                " vulkan/graphical_user_interface/gui_dll.cpp\n)\n", encoding="utf-8")
            previous = gate.REPO
            try:
                gate.REPO = root
                self.assertEqual(["runtime/engine.cpp"],
                                 [path.relative_to(root).as_posix() for path in gate.engine_sources()])
                self.assertEqual(["runtime/engine.cpp: 1 -> type:VkDevice"], gate.check_tokens(False))
            finally:
                gate.REPO = previous


if __name__ == "__main__":
    unittest.main()
