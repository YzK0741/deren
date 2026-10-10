#!/usr/bin/env python3
"""Does deren.exe still contain Vulkan? Layer 3: the binary's own bytes.

The import table (layer 1) and the unresolved symbols (layer 2) are checked by
`source/scripts/check_native_boundary.py`; this looks INSIDE the image for names and message text that came from
Vulkan - a log line that says "vulkan", a type name in an error string, a resolved entry point name handed to
`GetProcAddress`, a shader file name. Anything found here is VOCABULARY, not a dependency: it cannot link
Vulkan by itself, but it is exactly what P-Census (engine SOURCE vocabulary) is driving to zero.

Usage: python source/scripts/gui/x2_binary_scan.py [path-to-exe]
"""

from __future__ import annotations

import pathlib
import re
import sys

PATTERNS = {
    "vulkan (any case)": re.compile(rb"vulkan", re.I),
    "entry-point names (vkCmd*, vkGet*, vkCreate*)": re.compile(rb"vk[A-Z][A-Za-z]{2,}"),
    "type names (Vk*)": re.compile(rb"Vk[A-Z][A-Za-z]{2,}"),
    "macro names (VK_*)": re.compile(rb"VK_[A-Z0-9_]{3,}"),
    "shader / SPIR-V": re.compile(rb"spirv|\.spv\b", re.I),
}


def main() -> int:
    path = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "build-release-dyn-clang64/deren.exe")
    data = path.read_bytes()
    print(f"{path}: {len(data):,} bytes")
    for label, pattern in PATTERNS.items():
        # a string in the image is NUL-terminated: keep the ASCII runs the pattern matched inside
        hits = [m.decode("latin-1") for m in pattern.findall(data)]
        unique = sorted(set(hits), key=str.lower)
        print(f"  {label}: {len(hits)} occurrences, {len(unique)} distinct")
        if unique:
            print("     " + ", ".join(unique[:16]) + (" ..." if len(unique) > 16 else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
