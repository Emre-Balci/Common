#!/usr/bin/env bash
# Usage: ndk-check.sh <name> <relative .cpp path>...
# Compiles (no link) the given files of the private copy with the Android NDK r29 clang and libc++
# for arm64-v8a and x86_64 at API level 28, with the project's warnings and -Werror.
set -uo pipefail
SCRATCH="${SCRATCH:-/tmp/claude-0/-mnt-c-Repositories/e917f5b7-20cb-424d-ae71-1d43ba01b9bb/scratchpad}"  # export SCRATCH=<your scratch directory> to override
NDK=/root/Android/Sdk/ndk/29.0.14206865
CXX="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang++"
name="${1:?usage: ndk-check.sh <name> <file.cpp>...}"
shift
work="$SCRATCH/work/$name"
warnings='-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wold-style-cast -Wcast-align -Wcast-qual -Wdouble-promotion -Wfloat-equal -Wnull-dereference -Wundef -Wnon-virtual-dtor -Woverloaded-virtual -Wswitch-enum -Wimplicit-fallthrough -Wformat=2 -Wmissing-declarations -Wvla'
status=0
for target in aarch64-linux-android28 x86_64-linux-android28; do
    for file in "$@"; do
        if ! "$CXX" --target=$target -std=c++17 $warnings -Werror -ffp-contract=off -fno-exceptions -fno-rtti -O2 \
                -I"$work/include" -I"$work/tests" -I"$work/src" -c "$work/$file" -o /dev/null; then
            echo "NDK FAILED: $target $file"
            status=1
        fi
    done
done
[ $status -eq 0 ] && echo "NDK CHECK OK"
exit $status
