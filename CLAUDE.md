# common — notes for Claude

A C++17 real-time foundation library for Linux. It is the base for automotive and trading
projects. See README.md for the components and the rules.

## Layout
Headers are grouped by purpose in `include/common/<group>/`: `core`, `numeric`, `containers`,
`concurrency`, `time`, `system`, `diagnostics`, `control`, `protocol`, `file`. Sources mirror them in
`src/<group>/` (internal helpers in `src/detail/`), tests in `tests/<group>/`. A new component goes
in the matching group and is added to `CMakeLists.txt`, `tests/CMakeLists.txt`,
`include/common/common.hpp` and the README tables.

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
