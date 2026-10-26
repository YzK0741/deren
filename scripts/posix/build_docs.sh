#!/bin/sh
# Build the project documentation. Forwarding shim: the unified entry point is
# scripts/unified/build_docs.py, which also works on Windows.
#
# Usage:
#     sh scripts/posix/build_docs.sh

set -eu

# Resolve the repo root without depending on the external dirname utility.
case "$0" in
    */*) script_dir=${0%/*} ;;
    *) script_dir=. ;;
esac
root=$(CDPATH= cd -- "$script_dir/../.." && pwd)
for py in python3 python; do
    if command -v "$py" >/dev/null 2>&1; then
        exec "$py" "$root/scripts/unified/build_docs.py" "$@"
    fi
done
echo "error: Python 3.8+ not found (install python3 or run scripts/unified/build_docs.py explicitly)." >&2
exit 1
