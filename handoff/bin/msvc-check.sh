#!/usr/bin/env bash
# Usage: msvc-check.sh <name> <relative .cpp path>...
# Compiles (no link) the given files of the private copy $SCRATCH/work/<name> with MSVC 19.32 and the
# project's MSVC flags (cmake/CommonOptions.cmake: /W4 + extra warnings, /WX, /permissive-, no
# exceptions/RTTI), once with /Od and once with /O2. cmd.exe cannot see Linux paths, so the sources
# are copied under the repository's git-ignored build/ directory first.
set -uo pipefail
SCRATCH="${SCRATCH:-/tmp/claude-0/-mnt-c-Repositories/e917f5b7-20cb-424d-ae71-1d43ba01b9bb/scratchpad}"  # export SCRATCH=<your scratch directory> to override
REPO=/mnt/c/Repositories/Common
name="${1:?usage: msvc-check.sh <name> <file.cpp>...}"
shift
work="$SCRATCH/work/$name"
win="$REPO/build/msvc-check/$name"
rm -rf "$win" && mkdir -p "$win"
rsync -a "$work/include" "$work/src" "$work/tests" "$work/examples" "$win/"
flags='/nologo /c /std:c++17 /W4 /w14061 /w14062 /w14242 /w14254 /w14263 /w14265 /w14287 /w14296 /w14365 /w14388 /w14668 /w14826 /w15038 /w15262 /wd4324 /external:anglebrackets /external:W0 /WX /permissive- /Zc:__cplusplus /utf-8 /fp:precise /EHs-c- /GR- /D_HAS_EXCEPTIONS=0 /I include /I tests /I src'
{
    echo '@echo off'
    echo 'call "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat" >nul'
    echo 'if errorlevel 1 exit /b 1'
    for optimisation in /Od /O2; do
        for file in "$@"; do
            echo "echo [msvc $optimisation] $file"
            echo "cl $flags $optimisation /Fo:check.obj $file"
            echo 'if errorlevel 1 exit /b 1'
        done
    done
    echo 'echo MSVC CHECK OK'
} > "$win/check.bat"
cd "$win" && timeout 900 cmd.exe /c check.bat 2>&1 | tr -d '\r'
status=${PIPESTATUS[0]}
cd / && rm -rf "$win"
exit $status
