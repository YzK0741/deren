"""The tidy driver must use real target commands, not CMake's unbuilt synthetic BMIs."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("tidy_driver", ROOT / "scripts/tools/run_clang_tidy.py")
driver = importlib.util.module_from_spec(spec)
spec.loader.exec_module(driver)


class PrimaryCommands(unittest.TestCase):
    def test_skip_synthetic_record_even_when_first(self):
        records = [
            {"file": "module.cppm", "command": "clang++ -o CMakeFiles/lib@synth_0.dir/module.bmi"},
            {"file": "module.cppm", "command": "clang++ -fno-exceptions -o CMakeFiles/lib.dir/module.obj"},
        ]
        self.assertEqual(driver.primary_commands(records), [records[1]])

    def test_deduplicate_primary_keep_first(self):
        record = {"file": "module.cppm", "command": "clang++ -o module.obj"}
        self.assertEqual(driver.primary_commands([record, record.copy()]), [record])

    def test_arguments_form_and_windows_paths(self):
        records = [
            {"file": "D:/repo/a.cppm", "arguments": ["clang++", "-o", "lib@synth_0.dir/a.bmi"]},
            {"file": "D:/repo/a.cppm", "arguments": ["clang++", "-o", "a.obj"]},
        ]
        self.assertEqual(driver.primary_commands(records), [records[1]])


if __name__ == "__main__":
    unittest.main()
