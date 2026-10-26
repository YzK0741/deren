#!/bin/sh
# Run deren on POSIX. Forwarding shim: the unified entry point is
# scripts/unified/run.py. DEREN_CONFIG is still honoured for the old workflow.
#
# Usage:
#     sh scripts/posix/run.sh
#     sh scripts/posix/run.sh path/to/model.glb
#     sh scripts/posix/run.sh --config my.toml

set -eu

# Resolve the repo root without depending on the external dirname utility.
case "$0" in
    */*) script_dir=${0%/*} ;;
    *) script_dir=. ;;
esac
root=$(CDPATH= cd -- "$script_dir/../.." && pwd)
if [ -n "${DEREN_CONFIG:-}" ]; then
    set -- --type "$DEREN_CONFIG" "$@"
fi
for py in python3 python; do
    if command -v "$py" >/dev/null 2>&1; then
        exec "$py" "$root/scripts/unified/run.py" "$@"
    fi
done
echo "error: Python 3.8+ not found (install python3 or run scripts/unified/run.py explicitly)." >&2
exit 1
