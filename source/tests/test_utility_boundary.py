"""Keep utility below the renderer contract and engine layers."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


class UtilityBoundaryTests(unittest.TestCase):
    def test_utility_has_no_renderer_imports(self):
        forbidden = re.compile(r"^\s*(?:export\s+)?import\s+deren\.(?:promise|engine|vulkan)(?:[.;:]|\s)", re.M)
        for path in (ROOT / "source/utility").rglob("*"):
            if path.suffix in {".cpp", ".cppm"}:
                with self.subTest(source=str(path.relative_to(ROOT))):
                    self.assertIsNone(forbidden.search(path.read_text(encoding="utf-8")))

    def test_static_utility_has_no_contract_link(self):
        cmake = re.sub(r"#[^\n]*", "", (ROOT / "CMakeLists.txt").read_text(encoding="utf-8"))
        links = re.findall(r"target_link_libraries\(static_utility\s+(.*?)\)", cmake, re.S)
        self.assertTrue(links)
        for link in links:
            self.assertNotRegex(link, r"\b(?:promise|rhi_sync_contract|deren_engine)\b")


if __name__ == "__main__":
    unittest.main()
