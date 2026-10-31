"""Extract the pinned Boost.Graph header subset with upstream BCP, or verify it.

Run extraction into a fresh directory; no downloads or in-place vendor updates.
The source root must contain only a Boost release tree, not system headers.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

VERSION = 109200
HEADERS = [
    "boost/graph/adjacency_list.hpp",
    "boost/graph/breadth_first_search.hpp",
    "boost/graph/reverse_graph.hpp",
    "boost/graph/filtered_graph.hpp",
    "boost/throw_exception.hpp",
    "boost/version.hpp",
]


def hashes(root):
    return {
        path.relative_to(root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in sorted((root / "boost").rglob("*"))
        if path.is_file() and path.name != "int128.hpp" and "int128" not in path.relative_to(root / "boost").parts
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--boost-root", type=Path)
    parser.add_argument("--bcp", type=Path)
    parser.add_argument("--verify", action="store_true")
    args = parser.parse_args()
    output = args.output.resolve()
    if args.verify:
        manifest = json.loads((output / "headers.json").read_text(encoding="utf-8"))
        if manifest["boost_version"] != VERSION or manifest["entry_headers"] != HEADERS:
            raise SystemExit("Unexpected Boost version or entry-header set")
        actual = hashes(output)
        if actual != manifest["files"]:
            changed = sorted(set(actual) ^ set(manifest["files"]) | {
                path for path in actual.keys() & manifest["files"].keys()
                if actual[path] != manifest["files"][path]
            })
            raise SystemExit(f"Vendor mismatch: {changed[:10]}")
        license_hash = hashlib.sha256((output / "LICENSE_1_0.txt").read_bytes()).hexdigest()
        if license_hash != manifest["license_sha256"]:
            raise SystemExit("Vendor license mismatch")
        print(f"Verified Boost 1.92.0: {len(actual)} byte-identical headers")
        return
    if not args.boost_root or not args.bcp:
        parser.error("extraction requires --boost-root and --bcp")
    source = args.boost_root.resolve()
    version = (source / "boost/version.hpp").read_text(encoding="utf-8")
    if not re.search(r"^#define BOOST_VERSION 109200\s*$", version, re.MULTILINE):
        raise SystemExit("Expected Boost 1.92.0")
    if output.exists() and any(output.iterdir()):
        raise SystemExit("Output must be empty; extract to a staging directory")
    license_file = source / "LICENSE_1_0.txt"
    if not license_file.is_file():
        raise SystemExit("Boost root must include LICENSE_1_0.txt")
    output.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(
        [str(args.bcp.resolve()), f"--boost={source}", *HEADERS, str(output)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=True,
    )
    (output / "bcp.log").write_bytes(result.stdout)
    shutil.copyfile(license_file, output / "LICENSE_1_0.txt")
    files = hashes(output)
    if not files:
        raise SystemExit("BCP produced no headers")
    # Ensure the extractor copied the original bytes, without rewriting namespaces.
    for name, digest in files.items():
        if hashlib.sha256((source / name).read_bytes()).hexdigest() != digest:
            raise SystemExit(f"BCP modified header: {name}")
    manifest = {
        "boost_version": VERSION,
        "entry_headers": HEADERS,
        "license_sha256": hashlib.sha256(license_file.read_bytes()).hexdigest(),
        "files": files,
    }
    (output / "headers.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Extracted {len(files)} headers into {output}")


if __name__ == "__main__":
    main()
