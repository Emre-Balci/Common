# common — notes for Claude

A C++17 real-time foundation library for Linux that also builds and runs on Windows (MSVC), Android
(64-bit, API level 28+), macOS and iOS. It is the base for automotive and trading projects. See
README.md for the components and the rules.

## Layout
The project builds two libraries whose headers share the include root, grouped by purpose in
`include/common/<group>/`:
- `common-base` (`common::base`), the real-time foundation: `core`, `numeric`, `containers`,
  `concurrency`, `time`, `system`, `diagnostics`, `control`, `protocol`, `file`;
- `common` (`common::common`), patterns for building applications on it: `functional`, `object`,
  `event`, `state`, `task`, `app`. It links `common-base` publicly.

Sources mirror the groups in `src/<group>/` (internal helpers of `common-base` in `src/detail/`),
tests in `tests/<group>/`. A new component goes in the matching group and is added to its library in
`CMakeLists.txt`, to its test program in `tests/CMakeLists.txt` (`common_base_tests` or
`common_tests`), to its umbrella header (`include/common/base.hpp` or `include/common/common.hpp`)
and to the README tables. A new group of `common` also goes into `COMMON_GROUPS` in
`CMakeLists.txt`. `common-base` never includes a header of `common`: the `common_base_layering` test
fails if one of its files does.

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

macOS and iOS (`__APPLE__`) take the POSIX code too, but lack calls of Linux beyond POSIX. Where
portable POSIX code can stand in (`poll` for `ppoll`, `nanosleep` for `clock_nanosleep`, touching
pages for `MAP_POPULATE`), it is chosen by `COMMON_LINUX_API` (`core/config.hpp`), so that the
`portable` preset (`COMMON_PORTABLE_POSIX`) runs it on Linux under the tests. Apple's own calls
(`F_FULLFSYNC`, `F_PREALLOCATE`, `pthread_setname_np(name)`) are in `#if defined(__APPLE__)`
blocks. There is no Mac here: those blocks are only reviewed, and the README's table lists them.

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

## Rules learned building common (verified on all toolchains)
- A class derived from `Task`, `Unit` or `Object` is `final`, or declares a protected non-virtual
  destructor and no friends. A `friend` in a polymorphic class with a protected destructor makes GCC's
  `-Wnon-virtual-dtor` fail: give other classes access through a passkey (see `detail::TaskKey`).
- A type published through `SeqLock` is trivial (no default member initialisers): GCC 9's
  `-Wclass-memaccess` rejects the memcpy otherwise.
- Write defaulted default constructors as `X() noexcept = default;` (MSVC 19.32 deduces
  potentially-throwing for `std::array<ClassType, N>` members), and test conditions on non-type
  template parameters with `if constexpr` (MSVC C4127).
- A static data member template of a class template is MSVC C4268: put such tables in a nested class
  template (see `detail::inplace_function::Thunks`).
- GCC 9's `-Wshadow` flags a function-pointer parameter named like a member function; Clang's
  `-Wunused-lambda-capture` flags capturing a constant.
- libc++ (the NDK) does not mark `std::string_view(const char*)` noexcept: pass a `string_view`, not a
  literal, where a construction must be nothrow (e.g. `ObjectPool::create`).
- Do not name anything `emit`, `signals`, `slots` or `foreach` (Qt macros); `Signal::notify()`.
- Status codes: `kAlreadyExists` for a duplicate registration (or a task already in a runner),
  `kAlreadyRunning`/`kNotRunning` for start/stop, `kInvalidArgument` for a bad argument or a call in
  the wrong state.

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

For macOS and iOS, run the `portable` preset (it must pass too). On a Mac: `macos-debug`,
`macos-release` and `ios`.
