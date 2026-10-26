#!/bin/sh
# Build deren on POSIX, Windows/any platform with Python 3.8+.
# Forwarding shim: the unified entry point is scripts/unified/build.py.
#
# Usage:
#     sh scripts/posix/build.sh                 # Debug + Release
#     sh scripts/posix/build.sh Debug           # one config
#     sh scripts/posix/build.sh Release Clean   # clean rebuild

set -eu

# Resolve the repo root without depending on the external dirname utility.
case "$0" in
    */*) script_dir=${0%/*} ;;
    *) script_dir=. ;;
esac
root=$(CDPATH= cd -- "$script_dir/../.." && pwd)
for py in python3 python; do
    if command -v "$py" >/dev/null 2>&1; then
        exec "$py" "$root/scripts/unified/build.py" "$@"
    fi
done
echo "error: Python 3.8+ not found (install python3 or run scripts/unified/build.py explicitly)." >&2
exit 1
