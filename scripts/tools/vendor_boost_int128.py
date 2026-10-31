"""Copy a pinned official Boost.Int128 Git snapshot, or verify the vendored bytes offline."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

REVISION = "16f0b3374470e3821c10277775de18cf320cda45"


MANIFEST = "int128-headers.json"


def hashes(root):
    paths = sorted((root / "boost" / "int128").rglob("*")) + [root / "boost" / "int128.hpp"]
    return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in paths if p.is_file()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--source", type=Path)
    parser.add_argument("--verify", action="store_true")
    args = parser.parse_args()
    output = args.output.resolve()
    if args.verify:
        manifest = json.loads((output / MANIFEST).read_text(encoding="utf-8"))
        if manifest["revision"] != REVISION or manifest["files"] != hashes(output):
            raise SystemExit("Boost.Int128 revision or file hashes differ from manifest")
        print(f"Boost.Int128: {len(manifest['files'])} upstream files verified")
        return
    if args.source is None:
        parser.error("--source is required for extraction")
    if output.exists():
        raise SystemExit("Extraction requires a new output directory")
    source = args.source.resolve()
    revision = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    if revision != REVISION:
        raise SystemExit("Source must be checked out at the pinned revision")
    paths = subprocess.check_output(["git", "-C", str(source), "ls-tree", "-r", "--name-only", REVISION], text=True).splitlines()
    selected = [p for p in paths if p.startswith("include/boost/int128/") or p == "include/boost/int128.hpp" or p == "LICENSE"]
    if not selected or "LICENSE" not in selected:
        raise SystemExit("Missing upstream headers/license")
    for name in selected:
        if name == "LICENSE":
            continue
        target = output / ("boost/" + name.removeprefix("include/"))
        target.parent.mkdir(parents=True, exist_ok=True)
        # Read Git blobs, not autocrlf-converted working-tree files. Ignore local modifications.
        target.write_bytes(subprocess.check_output(["git", "-C", str(source), "show", f"{REVISION}:{name}"]))
    manifest = {"upstream": "https://github.com/boostorg/int128", "revision": REVISION, "files": hashes(output)}
    (output / MANIFEST).write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Boost.Int128: extracted {len(selected)} upstream files")


if __name__ == "__main__":
    main()
