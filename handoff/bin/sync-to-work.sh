#!/usr/bin/env bash
# Usage: sync-to-work.sh <name>
# Makes (or refreshes) a private copy of the shared repository at $SCRATCH/work/<name>, without .git
# and build directories. Build directories already in the copy are kept, so rebuilds are incremental.
set -euo pipefail
REPO=/mnt/c/Repositories/Common
SCRATCH="${SCRATCH:-/tmp/claude-0/-mnt-c-Repositories/e917f5b7-20cb-424d-ae71-1d43ba01b9bb/scratchpad}"  # export SCRATCH=<your scratch directory> to override
name="${1:?usage: sync-to-work.sh <name>}"
work="$SCRATCH/work/$name"
mkdir -p "$work"
rsync -a --delete --exclude=/.git/ --exclude=/build/ --exclude=/.cache/ --exclude=/compile_commands.json "$REPO/" "$work/"
echo "$work"
