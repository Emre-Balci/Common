# common — notes for Claude

A C++17 real-time foundation library for Linux that also builds and runs on Windows (MSVC) and
Android (64-bit, API level 28+). It is the base for automotive and trading projects. See README.md
for the components and the rules.

## Layout
Headers are grouped by purpose in `include/common/<group>/`: `core`, `numeric`, `containers`,
`concurrency`, `time`, `system`, `diagnostics`, `control`, `protocol`, `file`. Sources mirror them in
`src/<group>/` (internal helpers in `src/detail/`), tests in `tests/<group>/`. A new component goes
in the matching group and is added to `CMakeLists.txt`, `tests/CMakeLists.txt`,
`include/common/common.hpp` and the README tables.

## Platforms
Platform code sits in `#if defined(_WIN32)` / `#else` blocks next to the POSIX code, so both stay
in step. A component that is mostly platform-specific has `<name>.cpp` (shared parts),
`<name>_posix.cpp` and `<name>_win32.cpp`, chosen in `CMakeLists.txt` (`thread`, `serial_port`).
Windows helpers live in `src/detail/` (`win32.hpp`: `<windows.h>` and UTF-8 paths;
`win32_status.hpp`: error mapping that matches `posix_status.hpp`; `overlapped_io`). Public headers
never include `<windows.h>`; OS handles appear as `NativeHandle`. Keep the Linux behaviour
unchanged when adding Windows code, and document every Windows difference in the header.

Android is Linux with bionic in place of glibc, so it takes the POSIX code; where bionic lacks a call
(`pthread_setaffinity_np`, `feenableexcept`), an `#if defined(__ANDROID__)` block (or a check for
`__GLIBC__`) says what it does instead, and the header and the README's Android table say so too.

## Non-negotiable rules for library code (`include/`, `src/`)
- No heap allocation on the RT path: no `new`, `std::vector`, `std::string`, `std::function`
  or growing containers. Capacity is a template parameter.
- Never block on the RT path: no mutexes, no condition variables, no I/O. Cross-thread data
  goes through `SpscQueue`, `MpmcQueue`, `SeqLock` or `TripleBuffer`.
- No exceptions. Everything is `noexcept`. Errors are returned as `Status`/`Result<T>`.
  Contract violations use `COMMON_ASSERT`.
  Don't call throwing std members (`bitset::set/test`, `vector::at`, `optional::value`).
- Every loop has an explicit bound, including retry/CAS loops.
- Time comes from `MonotonicClock` only, never `system_clock`/CLOCK_REALTIME.
- Floating point: no `==` on computed values (`-Wfloat-equal` is on), and no `-ffast-math`.
  Float→int conversions go through `saturating_cast`/`saturating_round`.
- MISRA C++:2023 style: fixed-width types, brace-init, explicit casts, `final` classes,
  `[[nodiscard]]`, and braces on every branch. Any new deviation gets a
  `// Deviation (MISRA C++:2023 Rule X.Y.Z): reason` comment and a row in the README table.
- Match the existing style: 4-space indent, 120 columns, `lower_case` functions,
  `CamelCase` types, `kConstant`, `member_`.

## Build & test
Build in WSL (Ubuntu-24.04). When calling `wsl` from Git Bash on Windows, set `MSYS_NO_PATHCONV=1`.
For loops or variables, put the commands in a script file, because `wsl.exe` mangles `$var`.

```bash
cmake --preset gcc-debug && cmake --build --preset gcc-debug && ctest --preset gcc-debug
```
Before finishing a change, run `gcc-release`, `clang-release`, `asan` and `tsan` too. All of
them must build warning-free (`-Werror`) and pass. The FIFO test case is only fully exercised
when the tests run as root (`wsl -u root`).

For Windows, run `msvc-debug`, `msvc-release` and `msvc-asan` (also warning-free, `/WX`). From WSL,
put the commands in a `.bat` file that first calls `vcvars64.bat` of Visual Studio 2022 (find it
with `vswhere.exe -latest -property installationPath`, under `VC\Auxiliary\Build`), then run it
with `cmd.exe /c` from the repository on a `/mnt/<drive>` path. `msvc-release` matters: MSVC 19.40
has miscompiled code at `/O2` that passed in Debug.

For Android, set `ANDROID_NDK_HOME` and build `android-arm64` and `android-x86_64` (also
warning-free), and run `ctest --preset android-x86_64`: its executables are static, so the tests run
on the Linux host with bionic.
