#!/usr/bin/env bash
# Usage: build.sh <name> <preset> [test-filter] [target]
# Configures (once) and builds the private copy $SCRATCH/work/<name> with one of the project's CMake
# presets (gcc-debug, gcc-release, clang-debug, clang-release, asan, tsan, portable), builds <target>
# (default: common_tests) with 2 parallel jobs (memory is limited: never more), and runs it with the
# optional test-name filter (a substring). Prints the last lines of the build log on failure.
set -uo pipefail
SCRATCH="${SCRATCH:-/tmp/claude-0/-mnt-c-Repositories/e917f5b7-20cb-424d-ae71-1d43ba01b9bb/scratchpad}"  # export SCRATCH=<your scratch directory> to override
name="${1:?usage: build.sh <name> <preset> [filter] [target]}"
preset="${2:?usage: build.sh <name> <preset> [filter] [target]}"
filter="${3:-}"
target="${4:-common_tests}"
work="$SCRATCH/work/$name"
cd "$work" || exit 1
log="$work/build-$preset.log"
if [ ! -f "build/$preset/build.ninja" ]; then
    cmake --preset "$preset" > "$log" 2>&1 || { tail -40 "$log"; echo "CONFIGURE FAILED ($preset)"; exit 1; }
fi
if ! cmake --build --preset "$preset" --target "$target" --parallel 2 > "$log" 2>&1; then
    grep -E "error|warning" "$log" | head -60
    echo "----- last lines of $log -----"
    tail -30 "$log"
    echo "BUILD FAILED ($preset)"
    exit 1
fi
echo "build ok ($preset, $target)"
exe="build/$preset/tests/$target"
if [ -x "$exe" ]; then
    if [ -n "$filter" ]; then "$exe" "$filter"; else "$exe"; fi
    status=$?
    echo "TESTS EXIT CODE: $status ($preset)"
    exit $status
fi
