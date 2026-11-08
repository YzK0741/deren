"""Keep utility below the renderer contract and engine layers."""
from pathlib import Path
import os
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
        manifest = Path(os.environ.get("VR_TARGET_MANIFEST", ROOT / "build" / "target_manifest.txt"))
        links = []
        for line in manifest.read_text(encoding="utf-8").splitlines():
            fields = line.split()
            if len(fields) >= 3 and fields[0] == "link" and fields[1] == "static_utility":
                links.extend(fields[2:])
        self.assertTrue(links)
        for link in links:
            self.assertNotRegex(link, r"\b(?:promise|rhi_sync_contract|deren_engine)\b")


if __name__ == "__main__":
    unittest.main()
