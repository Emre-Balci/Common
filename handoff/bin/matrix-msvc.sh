#!/usr/bin/env bash
# Usage: matrix-msvc.sh [preset...]
# Builds and tests the shared repository with the MSVC presets (default msvc-debug msvc-release
# msvc-asan) through a .bat that calls vcvars64.bat first, run with cmd.exe from the repository.
set -uo pipefail
SCRATCH="${SCRATCH:-/tmp/claude-0/-mnt-c-Repositories/e917f5b7-20cb-424d-ae71-1d43ba01b9bb/scratchpad}"  # export SCRATCH=<your scratch directory> to override
REPO=/mnt/c/Repositories/Common
presets=("$@")
[ ${#presets[@]} -eq 0 ] && presets=(msvc-debug msvc-release msvc-asan)
mkdir -p "$SCRATCH/matrix" "$REPO/build"
overall=0
for preset in "${presets[@]}"; do
    bat="$REPO/build/matrix-$preset.bat"
    {
        echo '@echo off'
        echo 'call "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat" >nul'
        echo 'if errorlevel 1 exit /b 1'
        echo "cmake --preset $preset || exit /b 1"
        echo "cmake --build --preset $preset || exit /b 1"
        echo "ctest --preset $preset || exit /b 1"
    } > "$bat"
    log="$SCRATCH/matrix/$preset.log"
    (cd "$REPO" && timeout 3600 cmd.exe /c "build\\matrix-$preset.bat") 2>&1 | tr -d '\r' > "$log"
    status=${PIPESTATUS[0]}
    warnings=$(grep -ciE "warning [A-Z]*[0-9]+" "$log")
    failed=$(grep -E "^\[ FAIL \]" "$log" | sed 's/\[ FAIL \] //' | tr '\n' ' ')
    echo "$preset: exit=$status warnings=$warnings failed=[${failed}]"
    rm -f "$bat"
    [ $status -ne 0 ] && overall=1
done
exit $overall
