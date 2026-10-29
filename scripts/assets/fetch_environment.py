"""Fetch pinned optional Sample Viewer HDRs with SHA256 checks and attribution files."""
import argparse
import hashlib
from pathlib import Path
import urllib.request

REVISION = "3a030b54c2e4f0f4d469eec2f98f47398c9a2a30"
FILES = {
    "neutral.hdr": "dad135da12b0bc55c488574bd56bf011407c9ed3040493f240528cdfb51782e8",
    "neutral.hdr.license": "1a7e08b5af456cc8c1cfaa9de0911983099abe8bf5ef0106c5f0b95f8ec0fb3c",
    "papermill.hdr": "bb8b25d8c6590083c92931d596d5f00215b01f0498af4ce79bc0204be6d4a316",
    "papermill.hdr.license": "61c98ad4ea8c60127478a62c047cbb2afa801dceb481fedc9dbf7375430c57f9",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True, help="normally <build>/environments")
    parser.add_argument("--reference-papermill", action="store_true", help="also fetch Papermill, CC-BY-NC-SA-3.0; reference/noncommercial use")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    for name, expected in FILES.items():
        if name.startswith("papermill") and not args.reference_papermill:
            continue
        target = args.output / name
        if target.exists():
            if hashlib.sha256(target.read_bytes()).hexdigest() != expected:
                raise SystemExit(f"Refusing to replace existing file with different content: {target}")
            continue
        url = f"https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Environments/{REVISION}/{name}"
        with urllib.request.urlopen(url, timeout=60) as response:
            data = response.read(32 * 1024 * 1024 + 1)
        if hashlib.sha256(data).hexdigest() != expected:
            raise SystemExit(f"SHA256 mismatch: {name}")
        target.write_bytes(data)
    print("Studio Neutral: Copyright 2020 Amazon, LLC; distributed by Khronos Group; CC-BY-4.0.")
    if args.reference_papermill:
        print("Papermill: Copyright 2024 HDRLabs, sampled by UX3D; CC-BY-NC-SA-3.0.")
    print("Keep each .hdr.license beside its HDR. Source: https://github.com/KhronosGroup/glTF-Sample-Environments")


if __name__ == "__main__":
    main()
