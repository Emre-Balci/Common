#!/usr/bin/env bash
# Usage: sync-back.sh <name> <relative-path>...
# Copies the given files (paths relative to the repository root) from the private copy back into the
# shared repository. Each file is written to a temporary name next to its target and renamed over it,
# so other agents never read a half-written file. Only copy the files you own.
set -euo pipefail
REPO=/mnt/c/Repositories/Common
SCRATCH="${SCRATCH:-/tmp/claude-0/-mnt-c-Repositories/e917f5b7-20cb-424d-ae71-1d43ba01b9bb/scratchpad}"  # export SCRATCH=<your scratch directory> to override
name="${1:?usage: sync-back.sh <name> <file>...}"
shift
work="$SCRATCH/work/$name"
for relative in "$@"; do
    source="$work/$relative"
    target="$REPO/$relative"
    [ -f "$source" ] || { echo "missing in private copy: $relative" >&2; exit 1; }
    mkdir -p "$(dirname "$target")"
    cp "$source" "$target.sync-tmp"
    mv -f "$target.sync-tmp" "$target"
    echo "copied back: $relative"
done
