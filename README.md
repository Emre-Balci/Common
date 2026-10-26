# common

A C++17 foundation library for real-time applications on Linux, such as automotive control
loops and trading systems. It provides the pieces a real-time program needs. On the
real-time path, none of them allocate, block or throw, and every loop has a fixed upper bound.

It also builds and runs on Windows with MSVC, for tools, simulators and development, where the
same code compiles on both platforms; see [Windows](#windows) for what differs there. And on
Android (64-bit, API level 28 or later), for apps; see [Android](#android).

## Components

Headers are grouped by purpose in `include/common/<group>/`, and `#include "common/common.hpp"`
pulls in everything. Sources and tests mirror the groups in `src/<group>/` and `tests/<group>/`.
The last column says whether a component may be used on the real-time path.

### core: foundations every other header builds on

| Header | Purpose | RT-path safe |
|---|---|---|
| `core/config.hpp` | Platform checks (Linux, Android 9 or later, or Windows, C++17, no `-ffast-math` or `/fp:fast`), cache-line and page size, `NativeHandle` (a descriptor, or a Windows `HANDLE`) | yes |
| `core/status.hpp` | `Status` (`[[nodiscard]]` enum) and `Result<T>`, used instead of exceptions | yes |
| `core/assert.hpp` | `COMMON_ASSERT`: never throws; calls a handler that must not allocate, then aborts | yes |

### numeric: arithmetic without undefined behaviour

| Header | Purpose | RT-path safe |
|---|---|---|
| `numeric/integer.hpp` | Overflow-checked (`Result<T>`) and saturating integer arithmetic, range-checked integer conversions | yes |
| `numeric/float.hpp` | FTZ/DAZ, FP exception flags and traps, tolerance compare, saturating float→int | yes |
| `numeric/decimal.hpp` | `Decimal<N>`: exact fixed-point decimals (prices, quantities) with checked arithmetic, an explicit `RoundingMode` for every rounding, and text parsing and formatting | yes |

### containers: fixed capacity, no heap

| Header | Purpose | RT-path safe |
|---|---|---|
| `containers/span.hpp` | Non-owning, bounds-checked view of contiguous elements (C++17 stand-in for `std::span`) | yes |
| `containers/static_vector.hpp` | Fixed-capacity vector with inline storage | yes |
| `containers/static_string.hpp` | Fixed-capacity NUL-terminated string; appends that do not fit fail as a whole | yes |
| `containers/object_pool.hpp` | O(1) fixed-capacity object pool, replaces `new`/`delete` | yes |
| `containers/ring_buffer.hpp` | Overwrite-oldest circular buffer for sliding windows | yes |
| `containers/intrusive_list.hpp` | O(1) doubly-linked list whose links live in the elements (one hook per list an element can be in), e.g. for elements from an `ObjectPool` | yes |

### concurrency: lock-free communication between threads

| Header | Purpose | RT-path safe |
|---|---|---|
| `concurrency/spsc_queue.hpp` | Wait-free bounded single-producer/single-consumer ring buffer | yes |
| `concurrency/mpmc_queue.hpp` | Lock-free bounded multi-producer/multi-consumer queue (Vyukov), bounded CAS retries | yes |
| `concurrency/seqlock.hpp` | Single writer, many readers, "latest value" channel (e.g. quotes, sensor state) | yes |
| `concurrency/triple_buffer.hpp` | Wait-free single-writer/single-reader "latest value" channel for any copyable type; reads never fail | yes |
| `concurrency/broadcast_queue.hpp` | Wait-free single-producer/multi-consumer broadcast; each consumer has its own cursor, and a slow one skips the oldest messages with an exact loss count | yes |
| `concurrency/spin.hpp` | `cpu_relax()` pause hint for spin loops | yes |

### time: monotonic time and periodic activation

| Header | Purpose | RT-path safe |
|---|---|---|
| `time/clock.hpp` | `MonotonicClock` (CLOCK_MONOTONIC), absolute `sleep_until`, drift-free `PeriodicTimer` | yes |
| `time/deadline.hpp` | `Deadline`: one absolute timeout shared by the steps of an operation, saturating, with `never()` | yes |
| `time/timer_wheel.hpp` | `TimerWheel`: many timeouts with O(1) schedule, cancel and reschedule; never fires early, and runs a bounded number of callbacks per `advance()` | yes |

### system: process and thread set-up

| Header | Purpose | RT-path safe |
|---|---|---|
| `system/thread.hpp` | `RtThread` with explicit policy/priority/affinity, `CpuSet`, current-thread setters | init phase |
| `system/memory.hpp` | `mlockall`, glibc malloc tuning, stack pre-faulting | init phase |

### diagnostics: measuring and supervising the real-time phase

| Header | Purpose | RT-path safe |
|---|---|---|
| `diagnostics/latency_histogram.hpp` | O(1) latency recording with min/mean/max/percentiles | `record()` yes |
| `diagnostics/watchdog.hpp` | `Heartbeat` beaten by a supervised thread, `Watchdog` deadline supervision with an expiry handler | `beat()`, `check()` yes |
| `diagnostics/logger.hpp` | `Logger`: a real-time thread queues compact binary log records (wait-free, no formatting); a non-real-time thread formats and writes them, and reports dropped records | `log()` yes, `drain()` no |

### control: signal conditioning and feedback control for cyclic loops

| Header | Purpose | RT-path safe |
|---|---|---|
| `control/pid_controller.hpp` | `PidController`: anti-windup, derivative on measurement, holds its output on faulty input | yes |
| `control/low_pass_filter.hpp` | First-order IIR filter, configurable by cut-off frequency | yes |
| `control/slew_rate_limiter.hpp` | Limits the rate of change of a command | yes |
| `control/hysteresis.hpp` | Two-threshold switch (Schmitt trigger) | yes |
| `control/debouncer.hpp` | Accepts a change of a binary input after N stable samples | yes |

### protocol: exchanging messages with other systems

| Header | Purpose | RT-path safe |
|---|---|---|
| `protocol/rate_limiter.hpp` | O(1) `TokenBucket` (GCRA) and exact `SlidingWindowLimiter` ("N messages per window") | yes |
| `protocol/sequence_tracker.hpp` | `SequenceTracker`: classifies sequence numbers and E2E alive counters as in order, gap, jump, duplicate, stale or invalid, with statistics | yes |
| `protocol/delimited_framer.hpp` | `DelimitedFramer`: reassembles delimiter-terminated frames (e.g. lines) from a byte stream, drops ignored bytes, truncates overlong frames and resynchronises; `ByteSet` | yes |
| `protocol/text.hpp` | Text matching (`trim`, `starts_with`, `find`, ...), checked decimal/hex parsing (`parse_hex`, `decode_hex_bytes`) and formatting into buffers or a `StaticString` | yes |
| `protocol/byte_order.hpp` | Bounds-checked big/little-endian field reads and writes, MSB/LSB-first bit tests | yes |
| `protocol/backoff.hpp` | `ExponentialBackoff`: doubling, capped, bounded retry delays (e.g. reconnecting a device) | yes |

### file: file operations for initialisation and non-RT threads

File and device operations block in the kernel (page cache, storage, serial lines), so they belong
to the initialisation phase or to a non-real-time thread that real-time threads feed through a queue.

| Header | Purpose | RT-path safe |
|---|---|---|
| `file/file.hpp` | `File`: owning POSIX file handle (full reads/writes, positional I/O, sync, preallocate) and file-system functions | no |
| `file/whole_file.hpp` | `read_file`/`read_text_file` into a caller's buffer (also /proc and /sys), `write_file`, and crash-safe `write_file_atomically` | no |
| `file/line_reader.hpp` | `LineReader`: line-by-line reading of a text file through a fixed buffer (`\n` and `\r\n`) | no |
| `file/mapped_file.hpp` | `MappedFile`: memory mapping set up during initialisation, e.g. for reference data or a persistent state block | reading a populated, locked mapping yes |
| `file/serial_port.hpp` | `SerialPort`: raw-mode UART/USB-serial/pty with baud, parity, stop bits, RTS/CTS and exclusive access; reads and writes bounded by a `Deadline`; reports an unplugged device as `kDisconnected` | no |

## Real-time rules this library follows (and you should too)

1. **Two phases.** Initialisation may allocate, block and fail. After it ends, the
   real-time phase only uses the operations marked RT-safe above.
2. **No heap on the RT path.** Containers have compile-time capacity. Queues and pools embed
   their storage, so give them static storage duration, not stack.
3. **No blocking.** Threads talk through `SpscQueue`, `MpmcQueue`, `SeqLock`, `TripleBuffer` and
   `BroadcastQueue`, never mutexes. A full queue returns `false`. The RT thread then drops the item
   or degrades; it never waits. An RT thread logs through a `Logger`, which queues the record and
   leaves formatting and I/O to a non-RT thread.
4. **No exceptions.** Everything is `noexcept`. Errors are `Status`/`Result<T>`, and contract
   violations go through `COMMON_ASSERT`. Library targets build with `-fno-exceptions -fno-rtti`.
5. **Explicit scheduling.** `RtThread` creates threads with `PTHREAD_EXPLICIT_SCHED`, so the policy,
   priority and affinity are set at creation and never inherited by accident. Insufficient
   privilege is reported as `kPermissionDenied`, never silently ignored.
6. **Monotonic time only.** `MonotonicClock` for timestamps. `PeriodicTimer` sleeps to absolute
   deadlines (`start + k·period`), so jitter does not accumulate. Overruns are detected and
   counted, and the schedule realigns without a catch-up burst.
7. **Floating point.**
   - `-ffast-math` is rejected at compile time.
   - `-ffp-contract=off` keeps results identical across builds.
   - Call `enable_flush_denormals()` on each RT thread to avoid denormal slowdowns.
   - Check the sticky flags with `test_fp_exceptions()`. In debug builds, `enable_fp_traps()`
     raises SIGFPE at the first faulty operation.
   - Compare with `approx_equal`, never `==`. Convert with `saturating_cast`/`saturating_round`,
     because a plain cast of an out-of-range value is undefined behaviour.
8. **Bounded loops.** Every loop has a compile-time or capacity bound, including CAS and seqlock
   retries and EINTR restarts.
9. **Integer overflow.** Signed overflow is undefined behaviour, and unsigned wrap-around is rarely
   intended. Where an operand can be out of range, use `checked_add/sub/mul/div` (they return
   `Result<T>` with `kOverflow`) or the `saturating_*` versions. Convert between integer types
   with `checked_narrow`/`saturating_narrow`.
10. **Supervision and fault containment.** Each RT thread beats a `Heartbeat` every cycle. A
    supervisor thread runs a `Watchdog` and reacts to an expiry, e.g. by commanding a safe state
    or cancelling orders. The filter, slew-rate limiter and PID controller skip a NaN or
    infinite sample and hold their output instead of propagating it.

See [examples/rt_periodic_loop.cpp](examples/rt_periodic_loop.cpp) for the whole pattern: a
1 kHz RT loop that measures its own wake-up latency, filters it, and hands data to a non-RT
thread that also supervises it with a watchdog.

## Building (WSL / Linux)

Requires CMake ≥ 3.21, Ninja, and GCC ≥ 9 or Clang ≥ 10.

```bash
cmake --preset gcc-debug          # also: gcc-release, clang-debug, clang-release, asan, tsan, tidy
cmake --build --preset gcc-debug
ctest --preset gcc-debug
./build/gcc-debug/examples/rt_periodic_loop 1 2000   # [cpu] [cycles]
```

Always run the `tsan` and `asan` presets after changing anything concurrent. The `tidy` preset
needs `sudo apt install clang-tidy`.

## Building (Windows)

Requires Visual Studio 2022 (MSVC 19.30 or later) with the "Desktop development with C++"
workload, which includes CMake and Ninja, on Windows 10 1803 or later (x64). In a Developer
PowerShell or Developer Command Prompt for VS 2022 (the other presets are `msvc-release` and
`msvc-asan`):

```bat
cmake --preset msvc-debug
cmake --build --preset msvc-debug
ctest --preset msvc-debug
build\msvc-debug\examples\rt_periodic_loop.exe 1 2000
```

Visual Studio and VS Code (CMake Tools) open the folder and offer the same presets; each host only
lists its own. The MSVC build uses `/W4` with extra warnings close to the GCC/Clang set, `/WX`,
`/permissive-`, `/fp:precise` (no contraction into FMA), and turns off exceptions and RTTI.
Run `msvc-release` too before finishing a change: MSVC's optimizer has produced wrong code that
the debug build did not show (see the comment in `protocol/rate_limiter.hpp`).

## Building (Android)

Requires the Android NDK (tested with r29) on a Linux host, with `ANDROID_NDK_HOME` set to it, e.g.
`~/Android/Sdk/ndk/29.0.14206865`:

```bash
cmake --preset android-arm64 && cmake --build --preset android-arm64     # phones and tablets
cmake --preset android-x86_64 && cmake --build --preset android-x86_64   # also runs the tests
ctest --preset android-x86_64
```

Both presets build for API level 28 (Android 9). `android-x86_64` links the executables
statically, so the tests run on the Linux host itself, with Android's C library (bionic) and
libc++, without an emulator. `adb push` them to a device to run them there.

## Using it in a project

```cmake
# As a git submodule or a copy in third_party/common:
add_subdirectory(third_party/common)

# Or fetched:
include(FetchContent)
FetchContent_Declare(common GIT_REPOSITORY <url-of-this-repo> GIT_TAG <tag>)
FetchContent_MakeAvailable(common)

target_link_libraries(my_app PRIVATE common::common common::options)
```

`common::options` applies the same warnings, `-Werror` (when building this project at top level),
`-ffp-contract=off` and `-fno-exceptions -fno-rtti` to your target. Leave it out if you need
different flags. Tests and examples are only built when this project is the top-level project.

With MSVC, `common::options` adds `/EHs-c- /GR-` and `_HAS_EXCEPTIONS=0`. CMake's default flags
contain `/EHsc /GR`, so cl warns (D9025) that it overrides them; remove them from
`CMAKE_CXX_FLAGS` in your project to silence that, as this project does for its own targets.

## Windows

Every component builds on Windows, and the tests pass there. What is Linux-only either has a
Windows equivalent or reports `Status::kNotSupported`:

| Component | On Windows |
|---|---|
| `MonotonicClock` | `QueryPerformanceCounter` (through `std::chrono::steady_clock`); `to_timespec()` is Linux-only |
| `sleep_until`, `PeriodicTimer` | A high-resolution waitable timer per thread, created by the thread's first sleep; never wakes early |
| `RtThread` and the current-thread functions | `kOther` only: Windows has no `SCHED_FIFO`/`SCHED_RR`, so those report `kNotSupported`. Affinity is limited to one processor group, and names are thread descriptions |
| `lock_process_memory`, `configure_allocator_for_rt` | `kNotSupported` (no `mlockall` or glibc malloc) |
| `enable_flush_denormals`, `enable_fp_traps` | MXCSR, and `_controlfp_s` (traps raise structured exceptions) |
| `File`, file-system functions | UTF-8 paths through the wide-character API; files shared like POSIX files; `rename_file` replaces open files where NTFS offers POSIX semantics; permission bits are checked but have no effect |
| `MappedFile` | `CreateFileMapping`/`MapViewOfFile`, `VirtualLock`; see `file/mapped_file.hpp` for the differences |
| `SerialPort` | COM ports (`"COM3"`), overlapped I/O cancelled at the `Deadline`; always exclusive; bytes with parity errors are passed on |
| `write_log_line_to_fd` | Takes a `HANDLE` (`NativeHandle`), e.g. `GetStdHandle(STD_ERROR_HANDLE)` |

**Windows is not a real-time operating system.** Without real-time policies, locked memory or
isolated cores, wake-up latency is in the range of milliseconds and unbounded. Use it for tools,
simulators and tests; deploy real-time code on Linux.

The serial tests cannot open a COM port without hardware, so on Windows they drive the overlapped
I/O through named pipes. To test a real port, set `COMMON_TEST_SERIAL_PORT` (e.g. `COM4`, or
`/dev/ttyUSB0` on Linux): `serial_port_device_from_environment` then opens it, changes its speed and
waits for input, without sending anything.

## Android

Every component builds for 64-bit Android (arm64-v8a, x86_64) from API level 28 (Android 9) on,
and the tests pass with bionic. The 32-bit ABIs are refused: their `time_t` and `off_t` have 32
bits. Android is Linux, so the Linux code applies, except:

| Component | On Android |
|---|---|
| `RtThread` | `ThreadConfig::affinity` reports `kNotSupported`: bionic cannot create a thread with an affinity, so the thread calls `set_current_thread_affinity()` (`sched_setaffinity`) itself |
| `RtThread` and the current-thread functions | `SCHED_FIFO`/`SCHED_RR` report `kPermissionDenied`: apps have no `CAP_SYS_NICE` |
| `enable_fp_traps` | `kNotSupported` (bionic has no `feenableexcept`) |
| `configure_allocator_for_rt` | `kNotSupported` (no glibc malloc) |
| `SerialPort` | Terminal devices an app may open: pseudo-terminals, and USB serial adapters only on rooted devices |

API level 28 is required because `pthread_attr_setinheritsched` came with it: without it, bionic
lets a `kOther` thread inherit the policy of its creator.

## Real-time privileges

`SCHED_FIFO`/`SCHED_RR` need `CAP_SYS_NICE` or an `rtprio` limit. A large `mlockall` needs
`CAP_IPC_LOCK` or a `memlock` limit. Choose one of these:

```bash
# a) Grant limits to a group (takes effect at next login session)
sudo groupadd -f realtime && sudo usermod -aG realtime "$USER"
printf '@realtime - rtprio 98\n@realtime - memlock unlimited\n' | sudo tee /etc/security/limits.d/99-realtime.conf

# b) Grant capabilities to one binary (the file must live on a Linux filesystem, not /mnt/<drive>)
sudo setcap cap_sys_nice,cap_ipc_lock+ep ./my_app
```

**WSL is for development only.** Its kernel is not PREEMPT_RT and runs under the Hyper-V
scheduler, so its latency numbers only show trends. For deployment, use a PREEMPT_RT kernel
with isolated cores (`isolcpus=`, `nohz_full=`, `rcu_nocbs=`), IRQ affinity moved off the RT
cores, and CPU frequency scaling and deep C-states disabled.

## MISRA C++

The code targets **MISRA C++:2023**, which is based on C++17. In practice that means:
- fixed-width integer types and explicit casts, with `-Wconversion -Wsign-conversion -Wold-style-cast` as errors
- braces on every branch and no implicit fall-through
- `noexcept` throughout and `[[nodiscard]]` results
- no dynamic memory, `final` classes, and no C-style arrays in the library
- the numerical value of a character is never used (Rule 7.0.3): characters are only compared with
  `==` and `!=`, digits are converted through lookup tables, and bytes and characters meet only as
  two views of the same storage

GCC/Clang warnings and `.clang-tidy` only *approximate* the rule set. Compliance must be
confirmed with a qualified checker, such as Helix QAC, Parasoft C++test, PC-lint Plus or
Polyspace.

Documented deviations (each is also marked in the code):

| Rule | Where | Rationale |
|---|---|---|
| 19.0.2 function-like macros | `COMMON_ASSERT` | Needs call-site file/line and the expression text |
| 15.1.3 implicit converting constructors | `Span` | Like `std::span`: a container or `Span<T>` can be passed where a span (of const) is expected; it only creates a view |
| 30.0.1 `<cstdio>` | `rename_file` (Linux) | `rename()` is declared in `<cstdio>`, but it is the POSIX call that replaces a file atomically; no stream I/O is used |
| 21.6.3 advanced memory management | `StaticVector`, `ObjectPool` | Placement new into owned, aligned inline storage; this is what removes heap use |
| 8.2.5 `reinterpret_cast` | `StaticVector`, `ObjectPool`, `RtThread` (Windows) | Access to objects created in raw storage (with `std::launder`) and an address range check; `_beginthreadex()` returns the thread's `HANDLE` as an integer |
| 21.2.4 `offsetof` | `rename_file` (Windows) | `FILE_RENAME_INFO` ends in a variable-length name, which starts at `offsetof(FILE_RENAME_INFO, FileName)` |
| 10.4.1 `asm` | `cpu_relax`, FPCR access (AArch64 only) | No intrinsic exists |

Tests and examples use `<cstdio>` and `std::thread` and are outside the compliance scope.
