"""Bounded-subprocess tests, plus the offline decoder's raw-record and exact-frame rules."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import unittest

SCRIPTS = Path(__file__).resolve().parents[2] / "scripts" / "diagnostics"
sys.path.insert(0, str(SCRIPTS))
import symbolize_stacktrace
from stacktrace_process import run_bounded

DECODER = SCRIPTS / "symbolize_stacktrace.py"

# A crash record with the two shapes that matter: the first frame IS the fault PC (0x2340 in
# C:\build\deren.exe), the second is a return address (0x1100).
RAW_CRASH = (
    "deren crash code=0xc0000005 fault_pc=0x50002340\n"
    "deren raw stack v1 frames=2 truncated=0\n"
    "frame pc=0x50002340 base=0x50000000 rva=0x2340 module=C:\\build\\deren.exe\n"
    "frame pc=0x50001100 base=0x50000000 rva=0x1100 module=C:\\build\\deren.exe\n"
)

# The SECOND frame repeats the first frame's PC and RVA. The crash handler records the exception
# address as frame 0, so frame 0 is exact and frame 1 - the very same number - is still a RETURN
# address and decodes one byte lower. Deciding by `pc == fault_pc` would hand 0x2340 out twice.
RAW_REPEATED_FAULT_PC = (
    "deren crash code=0xc0000005 fault_pc=0x50002340\n"
    "deren raw stack v1 frames=2 truncated=0\n"
    "frame pc=0x50002340 base=0x50000000 rva=0x2340 module=C:\\build\\deren.exe\n"
    "frame pc=0x50002340 base=0x50000000 rva=0x2340 module=C:\\build\\deren.exe\n"
)

# An ordinary panic trace: no crash line, so EVERY frame is a return address - the record holds no
# exact frame at all, and a first-frame flag must not leak into it.
RAW_PANIC = (
    "deren raw stack v1 frames=2 truncated=0\n"
    "frame pc=0x50002340 base=0x50000000 rva=0x2340 module=C:\\build\\deren.exe\n"
    "frame pc=0x50001100 base=0x50000000 rva=0x1100 module=C:\\build\\deren.exe\n"
)

# Two records concatenated into one log. Each record owns its first frame, so every `deren crash`
# line RESETS the positional state instead of pinning it to the first frame of the file.
RAW_TWO_RECORDS = (
    "deren crash code=0xc0000005 fault_pc=0x50002340\n"
    "deren raw stack v1 frames=1 truncated=0\n"
    "frame pc=0x50002340 base=0x50000000 rva=0x2340 module=C:\\build\\deren.exe\n"
    "deren crash code=0xc0000005 fault_pc=0x60001100\n"
    "deren raw stack v1 frames=2 truncated=0\n"
    "frame pc=0x60001100 base=0x60000000 rva=0x1100 module=C:\\build\\other.dll\n"
    "frame pc=0x60001200 base=0x60000000 rva=0x1200 module=C:\\build\\other.dll\n"
)


class ScratchDirectory:
    """A directory under the working directory, removed on exit.

    NOT `tempfile.TemporaryDirectory`: it creates its directory with owner-only permissions
    (0o700), and on Windows a process can then be refused when writing INSIDE it (measured:
    PermissionError from `Path.write_text`, and again from the cleanup) even though creating the
    directory and writing beside it both succeed. The working directory is where this suite
    already puts its artifacts - the C++ sibling tests write crash.log and fast.log there - so a
    plain directory with inherited permissions is both portable and inspectable on failure.
    """

    def __init__(self, name: str):
        self.path = Path.cwd() / f"stacktrace-offline-{name}"

    def __enter__(self) -> Path:
        shutil.rmtree(self.path, ignore_errors=True)
        self.path.mkdir(parents=True)
        return self.path

    def __exit__(self, *exc_info) -> bool:
        shutil.rmtree(self.path, ignore_errors=True)
        return False


class BoundedSymbolizerTests(unittest.TestCase):
    def test_success(self):
        self.assertEqual(run_bounded([sys.executable, "-c", "print('frame')"], os.environ.copy(), 2),
                         (True, b"frame\r\n" if os.name == "nt" else b"frame\n"))

    def test_output_limit(self):
        ok, output = run_bounded([sys.executable, "-c", "print('x'*131072)"], os.environ.copy(), 2, 1024)
        self.assertFalse(ok)
        self.assertLessEqual(len(output), 1024)

    def test_descendants_die_on_timeout_and_parent_exit(self):
        for parent_exits in (False, True):
            with self.subTest(parent_exits=parent_exits), ScratchDirectory("descendants") as directory:
                marker = directory / "escaped.txt"
                child = f"import time; time.sleep(1); open({str(marker)!r}, 'w').write('escaped')"
                parent = (f"import subprocess,sys,time; subprocess.Popen([sys.executable,'-c',{child!r}],"
                          "stdout=sys.stdout, stderr=sys.stderr); print('spawned',flush=True); "
                          + ("" if parent_exits else "time.sleep(10)"))
                ok, output = run_bounded([sys.executable, "-c", parent], os.environ.copy(), 0.4)
                self.assertEqual(ok, parent_exits)
                if parent_exits:
                    self.assertIn(b"spawned", output)
                time.sleep(1.2)
                self.assertFalse(marker.exists(), "descendant survived containment cleanup")


class ModuleNameTests(unittest.TestCase):
    """The archived-log lookup: a recorded module path reduces to its file name on ANY host."""

    def test_every_separator_yields_the_file_name(self):
        # A Windows record includes a path such as C:\Windows\SYSTEM32\ntdll.dll. `Path(...).name`
        # splits on the HOST separator only, so on POSIX that whole string is one name and the
        # lookup under --module-dir would silently miss. ntpath splits on both separators.
        name = symbolize_stacktrace.module_file_name
        self.assertEqual(name("C:\\Windows\\SYSTEM32\\ntdll.dll"), "ntdll.dll")
        self.assertEqual(name("/usr/lib/libc.so.6"), "libc.so.6")
        self.assertEqual(name("C:/build/deren.exe"), "deren.exe")
        self.assertEqual(name("C:/build\\deren.exe"), "deren.exe")
        self.assertEqual(name("deren.exe"), "deren.exe")


@unittest.skipUnless(os.name == "nt", "the fake symbolizer is a Windows command script")
class DecoderTests(unittest.TestCase):
    """symbolize_stacktrace.py: the raw records survive, the FIRST frame stays exact."""

    def _fake_tool(self, directory: Path, body: str) -> Path:
        tool = directory / "fake-symbolizer.cmd"
        tool.write_bytes(b"@echo off\r\n" + body.encode("ascii") + b"\r\nexit /b 0\r\n")
        return tool

    def _decoder_run(self, directory: Path, tool: Path, log_text: str = RAW_CRASH,
                     extra=None) -> subprocess.CompletedProcess:
        log = directory / "crash.log"
        log.write_text(log_text, encoding="utf-8", newline="")
        return subprocess.run([sys.executable, str(DECODER), str(log), "--symbolizer", str(tool), *(extra or [])],
                              capture_output=True, text=True)

    @staticmethod
    def _handed(result: subprocess.CompletedProcess):
        """The 0x... addresses the decoder handed the symbolizer, in frame order."""
        return [token
                for line in result.stdout.splitlines() if line.startswith("all=")
                for token in line.split()[1:]
                if token.startswith("0x")]

    def test_fault_pc_is_exact_and_return_pcs_are_adjusted(self):
        with ScratchDirectory("decoder-exact") as directory:
            # The tool echoes what the decoder handed it, which is the only way to observe the
            # adjustment without a real symbolizer. `%*` rather than a fixed `%7`: how many
            # leading arguments cmd.exe itself contributes is not this test's subject.
            tool = self._fake_tool(directory, "echo all=%*")
            result = self._decoder_run(directory, tool)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(self._handed(result), ["0x2340", "0x10ff"])  # first exact, rest - 1
            self.assertIn("frame pc=0x50002340", result.stdout)  # every raw line is preserved

    def test_a_later_frame_that_repeats_the_fault_pc_is_still_adjusted(self):
        # Value comparison cannot tell these two frames apart; their POSITION can.
        with ScratchDirectory("decoder-positional") as directory:
            tool = self._fake_tool(directory, "echo all=%*")
            result = self._decoder_run(directory, tool, RAW_REPEATED_FAULT_PC)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(self._handed(result), ["0x2340", "0x233f"])

    def test_without_a_crash_line_every_frame_is_adjusted(self):
        with ScratchDirectory("decoder-panic") as directory:
            tool = self._fake_tool(directory, "echo all=%*")
            result = self._decoder_run(directory, tool, RAW_PANIC)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(self._handed(result), ["0x233f", "0x10ff"])

    def test_elf_zero_load_bias_is_still_a_resolved_module(self):
        with ScratchDirectory("decoder-elf") as directory:
            tool = self._fake_tool(directory, "echo all=%*")
            raw = "frame pc=0x401100 base=0x0 rva=0x401100 module=/tmp/probe\n"
            result = self._decoder_run(directory, tool, raw)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(self._handed(result), ["0x4010ff"])

    def test_every_concatenated_record_has_its_own_exact_frame(self):
        with ScratchDirectory("decoder-records") as directory:
            tool = self._fake_tool(directory, "echo all=%*")
            result = self._decoder_run(directory, tool, RAW_TWO_RECORDS)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(self._handed(result), ["0x2340", "0x1100", "0x11ff"])

    def test_archived_windows_module_resolves_under_module_dir(self):
        # The log names C:\build\deren.exe; only its file name may be looked up in the archive.
        with ScratchDirectory("decoder-archive") as directory:
            tool = self._fake_tool(directory, "echo all=%*")
            (directory / "deren.exe").write_bytes(b"")  # the archived EXE the log names
            result = self._decoder_run(directory, tool, extra=["--module-dir", str(directory)])
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stderr, "")
            self.assertEqual(self._handed(result), ["0x2340", "0x10ff"])

    def test_a_missing_module_warns_once_and_still_fails(self):
        with ScratchDirectory("decoder-missing") as directory:
            tool = self._fake_tool(directory, "echo all=%*")
            archive = directory / "archive"
            archive.mkdir()
            result = self._decoder_run(directory, tool, extra=["--module-dir", str(archive)])
            self.assertNotEqual(result.returncode, 0, result.stdout)
            # ONE line per DISTINCT module: both frames name the same DLL.
            self.assertEqual(result.stderr.count("missing module file"), 1, result.stderr)
            self.assertIn("deren.exe", result.stderr)
            self.assertIn("frame pc=0x50001100", result.stdout)  # the raw record is still printed

    def test_clean_exit_without_output_is_a_failure(self):
        # Nothing decoded must not look like a complete decode: the raw records are still
        # printed, but the exit code says the run was partial.
        with ScratchDirectory("decoder-quiet") as directory:
            tool = self._fake_tool(directory, "rem a tool that writes nothing")
            result = self._decoder_run(directory, tool)
            self.assertEqual(result.returncode, 1, result.stdout)
            self.assertIn("frame pc=0x50001100", result.stdout)

    def test_the_budget_stops_the_whole_decode(self):
        # The tool sleeps for seconds; a 1 ms budget must end the run in milliseconds and say how
        # many frames were left undecoded. Without the budget the per-frame timeout would take
        # seconds per frame.
        with ScratchDirectory("decoder-budget") as directory:
            tool = self._fake_tool(directory, "ping -n 4 127.0.0.1 >nul\r\necho all=%*")
            started = time.monotonic()
            result = self._decoder_run(directory, tool, extra=["--budget", "0.001"])
            elapsed = time.monotonic() - started
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertLess(elapsed, 5, "the decode must stop at its budget, not at the tool")
            self.assertIn("not decoded", result.stderr)


if __name__ == "__main__":
    unittest.main()
