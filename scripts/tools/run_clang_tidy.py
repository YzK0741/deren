"""Run clang-tidy once per real target source using CMake's actual compile flags.

CMake also exports synthetic module-consumer commands. Their BMIs need not be built by
the selected targets, and their reduced flags are unsuitable for source diagnostics.
Keep the original working directory/response files, but exclude those duplicate records.
"""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile


def primary_commands(records):
    result = []
    seen = set()
    for record in records:
        command = record.get("command", " ".join(record.get("arguments", [])))
        if "@synth_" in command or "@synth_" in record.get("output", ""):
            continue
        source = record["file"].replace("\\", "/")
        if source not in seen:
            result.append(record)
            seen.add(source)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--clang-tidy", default="clang-tidy")
    parser.add_argument("--fix", action="store_true")
    parser.add_argument("files", nargs="+")
    args = parser.parse_args()
    database = args.build_dir.resolve() / "compile_commands.json"
    records = primary_commands(json.loads(database.read_text(encoding="utf-8")))
    sources = {Path(record["file"]).resolve() for record in records}
    files = [Path(file).resolve() for file in args.files]
    for file in files:
        if not file.is_file() or file not in sources:
            parser.error(f"no real compile command or source file for {file}")
    with tempfile.TemporaryDirectory(prefix="tidy-primary-", dir=args.build_dir.resolve()) as directory:
        (Path(directory) / "compile_commands.json").write_text(json.dumps(records), encoding="utf-8")
        command = [args.clang_tidy, "-p", directory, "--warnings-as-errors=*"]
        if args.fix:
            command.append("--fix")
        return subprocess.run(command + [str(file) for file in files], check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
