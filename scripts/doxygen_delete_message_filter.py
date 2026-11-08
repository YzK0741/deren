#!/usr/bin/env python3
"""Doxygen input filter for C++26 deleted-function messages.

Doxygen 1.18 stops parsing a class body at a deleted function that carries a
message, e.g. ``foo(foo const&) = delete("foo is non-copyable");``. The
declaration is valid C++26, but Doxygen then reports every later member as
"documented symbol was not declared or defined". This filter removes only the
message from the copy Doxygen reads; the compiler still sees the real source.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

_DELETE_MESSAGE = re.compile(r'=\s*delete\s*\(\s*"(?:[^"\\]|\\.)*"\s*\)')


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: doxygen_delete_message_filter.py <source-file>", file=sys.stderr)
        return 2
    source = Path(sys.argv[1]).read_text(encoding="utf-8")
    sys.stdout.write(_DELETE_MESSAGE.sub("= delete", source))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
