#!/usr/bin/env bash
# Usage: matrix-linux.sh [preset...]
# Builds every target of the shared repository with each Linux preset (default: all that CLAUDE.md
# requires) and runs ctest. Prints one summary line per preset; logs in $SCRATCH/matrix/<preset>.log.
set -uo pipefail
SCRATCH="${SCRATCH:-/tmp/claude-0/-mnt-c-Repositories/e917f5b7-20cb-424d-ae71-1d43ba01b9bb/scratchpad}"  # export SCRATCH=<your scratch directory> to override
REPO=/mnt/c/Repositories/Common
presets=("$@")
[ ${#presets[@]} -eq 0 ] && presets=(gcc-debug gcc-release clang-release asan tsan portable)
mkdir -p "$SCRATCH/matrix"
cd "$REPO" || exit 1
overall=0
for preset in "${presets[@]}"; do
    log="$SCRATCH/matrix/$preset.log"
    {
        cmake --preset "$preset" && cmake --build --preset "$preset" --parallel 4 && ctest --preset "$preset"
    } > "$log" 2>&1
    status=$?
    warnings=$(grep -c "warning:" "$log")
    failed=$(grep -E "^\[ FAIL \]" "$log" | sed 's/\[ FAIL \] //' | tr '\n' ' ')
    echo "$preset: exit=$status warnings=$warnings failed=[${failed}]"
    [ $status -ne 0 ] && overall=1
done
exit $overall
