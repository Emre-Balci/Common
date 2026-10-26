#!/usr/bin/env bash
# Usage: matrix-android.sh
# Builds android-arm64 and android-x86_64 (static executables) and runs ctest for android-x86_64 on
# this Linux host, with the NDK r29 in ~/Android/Sdk.
set -uo pipefail
SCRATCH="${SCRATCH:-/tmp/claude-0/-mnt-c-Repositories/e917f5b7-20cb-424d-ae71-1d43ba01b9bb/scratchpad}"  # export SCRATCH=<your scratch directory> to override
REPO=/mnt/c/Repositories/Common
export ANDROID_NDK_HOME=/root/Android/Sdk/ndk/29.0.14206865
mkdir -p "$SCRATCH/matrix"
cd "$REPO" || exit 1
overall=0
for preset in android-arm64 android-x86_64; do
    log="$SCRATCH/matrix/$preset.log"
    if [ "$preset" = android-x86_64 ]; then
        { cmake --preset "$preset" && cmake --build --preset "$preset" --parallel 4 && ctest --preset "$preset"; } > "$log" 2>&1
    else
        { cmake --preset "$preset" && cmake --build --preset "$preset" --parallel 4; } > "$log" 2>&1
    fi
    status=$?
    warnings=$(grep -c "warning:" "$log")
    failed=$(grep -E "^\[ FAIL \]" "$log" | sed 's/\[ FAIL \] //' | tr '\n' ' ')
    echo "$preset: exit=$status warnings=$warnings failed=[${failed}]"
    [ $status -ne 0 ] && overall=1
done
exit $overall
