"""Native POSIX integration; argv[1] is the compiled standalone probe."""
import os
from pathlib import Path
import resource
import signal
import subprocess
import sys
import tempfile
import unittest

PROBE = str(Path(sys.argv.pop(1)).resolve())
SYMBOLIZER = sys.argv.pop(1)
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def run(*args, env=None):
    with tempfile.TemporaryDirectory() as directory:
        result = subprocess.run([PROBE, *args], capture_output=True, text=True, timeout=4, env=env, cwd=directory)
        report = Path(directory) / "deren-crash.log"
        result.report = report.read_text() if report.is_file() else ""
        return result


class PosixStacktraceTests(unittest.TestCase):
    def test_capture(self):
        result = run("--capture")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("frame pc=0x", result.report)
        self.assertEqual(result.stderr, "")

    @unittest.skipUnless(Path(SYMBOLIZER).is_file(), "llvm-symbolizer unavailable")
    def test_symbolize_and_environment(self):
        env = os.environ.copy()
        env["LLVM_SYMBOLIZER_OPTS"] = "--deren-invalid-option"
        result = run("--symbols", SYMBOLIZER, env=env)
        self.assertIn("status=0", result.stdout)
        self.assertIn("posix_capture_site", result.stdout)
        self.assertIn("stacktrace_posix_probe.cpp:", result.stdout)

    def test_missing_tool(self):
        self.assertIn("status=1", run("--symbols", "/deren/no-such-tool").stdout)

    @unittest.skipUnless(Path(SYMBOLIZER).is_file(), "llvm-symbolizer unavailable")
    def test_offline_capture(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "trace.log"
            log.write_text(run("--capture").report)
            decoder = Path(__file__).resolve().parents[2] / "scripts" / "symbolize_stacktrace.py"
            result = subprocess.run([sys.executable, str(decoder), str(log), "--symbolizer", SYMBOLIZER],
                                    capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("posix_capture_site", result.stdout)

    def test_timeout_output_and_descendant_cleanup(self):
        with tempfile.TemporaryDirectory() as directory:
            tool = Path(directory) / "tool with spaces"
            marker = Path(directory) / "escaped"
            tool.write_text(f"#!/bin/sh\n(sleep 1; echo escaped > '{marker}') &\nsleep 10\n")
            tool.chmod(0o700)
            self.assertIn("status=2", run("--timeout", str(tool)).stdout)
            import time
            time.sleep(1.2)
            self.assertFalse(marker.exists())
            tool.write_text("#!/bin/sh\nyes x\n")
            self.assertIn("status=3", run("--limit", str(tool)).stdout)

    def test_signal_context_and_worker_overflow(self):
        for mode in ("--crash", "--overflow"):
            with self.subTest(mode=mode):
                result = run(mode)
                self.assertEqual(result.returncode, -signal.SIGSEGV, result.stderr)
                self.assertIn("deren crash signal=", result.report)
                self.assertEqual(result.stderr, "")
                self.assertRegex(result.report, r"fault_pc=0x[1-9a-f][0-9a-f]*")
                self.assertIn("unwind skipped", result.report)

    def test_fast_path_does_not_capture(self):
        result = run("--fast")
        self.assertEqual(result.returncode, -signal.SIGABRT)
        self.assertIn("minimal POSIX fatal", result.report)
        self.assertEqual(result.stderr, "")
        self.assertNotIn("deren crash", result.report)
        self.assertNotIn("frame pc=", result.report)


if __name__ == "__main__":
    unittest.main()
