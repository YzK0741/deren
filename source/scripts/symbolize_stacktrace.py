"""Decode deren raw stack v1 records using matching binaries and llvm-symbolizer.

Healthy/offline tool only. Never invoked by fast_panic or the crash handler.
"""
import argparse
import ntpath
import os
from pathlib import Path
import re
import shutil
import sys
import time
from stacktrace_process import run_bounded

FRAME = re.compile(r"^frame pc=0x([0-9a-fA-F]+) base=0x([0-9a-fA-F]+) rva=0x([0-9a-fA-F]+) module=(.*)$")


def module_file_name(module: str) -> str:
    """The file name of a recorded module path, on ANY platform.

    `Path(module).name` splits on the HOST separator only, and a Windows record stores `\\`: an
    archived `C:\\Windows\\SYSTEM32\\ntdll.dll` has no name at all on POSIX, so the lookup under
    `--module-dir` would silently miss. `ntpath` splits on both separators everywhere.
    """
    return ntpath.basename(module)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--symbolizer", default=shutil.which("llvm-symbolizer"))
    parser.add_argument("--module-dir", type=Path, help="Directory containing the exact archived EXE/DLL and symbols")
    parser.add_argument("--timeout", type=float, default=3.0, help="Timeout per frame in seconds (maximum 10)")
    parser.add_argument("--budget", type=float, default=30.0,
                        help="Time budget for the whole decode in seconds (maximum 600)")
    parser.add_argument("--no-inlines", action="store_true")
    args = parser.parse_args()
    if not 0 < args.timeout <= 10:
        parser.error("--timeout must be greater than zero and at most 10 seconds")
    if not 0 < args.budget <= 600:
        parser.error("--budget must be greater than zero and at most 600 seconds")
    deadline = time.monotonic() + args.budget
    failures = 0
    skipped = 0
    # The FIRST frame of a crash record IS the exception address and is EXACT: moving it one byte
    # back decodes the instruction BEFORE the fault. Every later frame holds a RETURN address and
    # is decoded at pc - 1. The in-process decoder names the same split with
    # `exact_address_count`, and it is positional: a later frame that happens to repeat the fault
    # PC is still a return address, while an ordinary panic trace (no crash line) has no exact
    # frame at all. Each crash line restarts the count, so concatenated records stay correct.
    exact_frames = 0
    missing = set()
    # The parent checks temporary-file size while the child runs, avoiding an
    # unbounded communicate() buffer if an invalid replacement tool floods stdout.
    with args.log.open("r", encoding="utf-8", errors="replace") as log:
        for raw in log:
            line = raw.removesuffix("\n")
            if line.startswith("deren crash "):
                exact_frames = 1
            print(line)
            frame = FRAME.fullmatch(line)
            if not frame:
                continue
            exact = exact_frames > 0
            if exact_frames:
                exact_frames -= 1
            # The recorded `pc` is deliberately NOT read: the exact/return split is positional.
            base, offset = (int(frame[i], 16) for i in (2, 3))
            module = frame[4]
            if not args.symbolizer or not module or not base:
                failures += 1
                continue
            if args.module_dir:
                path = args.module_dir / module_file_name(module)
                # A module that was not archived cannot be symbolized, so the frame FAILS here
                # instead of being handed to a child that can only fail on the missing file. One
                # line per distinct file: a deep stack names the same DLL many times.
                if not path.is_file():
                    if module not in missing:
                        missing.add(module)
                        print(f"warning: missing module file {path}", file=sys.stderr)
                    failures += 1
                    continue
            else:
                path = Path(module)
            address = offset if exact else max(0, offset - 1)
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                skipped += 1
                continue
            command = [args.symbolizer, f"--obj={path}", "--relative-address", "--functions=linkage", "--demangle",
                       "--no-debuginfod", "--no-inlines" if args.no_inlines else "--inlines", hex(address)]
            env = os.environ.copy()
            env.pop("LLVM_SYMBOLIZER_OPTS", None)
            try:
                # What is left of the whole-decode budget caps the per-frame timeout.
                success, output = run_bounded(command, env, min(args.timeout, remaining))
                # A clean exit with NO output decoded nothing. Counting it as a success would print
                # nothing while returning exit 0 - a partial decode would look complete, and a
                # PDB-less module would look like a frame that was symbolized.
                if success and output.strip():
                    print(output.decode("utf-8", errors="replace").rstrip())
                elif time.monotonic() >= deadline:
                    skipped += 1  # the whole-decode budget, not the tool, ended this frame
                else:
                    failures += 1
            except OSError:
                failures += 1
    if skipped:
        print(f"budget of {args.budget:g}s expired: {skipped} frame(s) not decoded", file=sys.stderr)
    return 1 if failures or skipped else 0


if __name__ == "__main__":
    raise SystemExit(main())
