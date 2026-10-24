# common

A C++17 foundation library for real-time applications on Linux, such as automotive control
loops and trading systems. It provides the pieces a real-time program needs. On the
real-time path, none of them allocate, block or throw, and every loop has a fixed upper bound.

## Components

| Header | Purpose | RT-path safe |
|---|---|---|
| `spsc_queue.hpp` | Wait-free bounded single-producer/single-consumer ring buffer | yes |
| `mpmc_queue.hpp` | Lock-free bounded multi-producer/multi-consumer queue (Vyukov), bounded CAS retries | yes |
| `seqlock.hpp` | Single writer, many readers, "latest value" channel (e.g. quotes, sensor state) | yes |
| `static_vector.hpp` | Fixed-capacity vector with inline storage | yes |
| `object_pool.hpp` | O(1) fixed-capacity object pool, replaces `new`/`delete` | yes |
| `ring_buffer.hpp` | Overwrite-oldest circular buffer for sliding windows | yes |
| `clock.hpp` | `MonotonicClock` (CLOCK_MONOTONIC), absolute `sleep_until`, drift-free `PeriodicTimer` | yes |
| `latency_histogram.hpp` | O(1) latency recording with min/mean/max/percentiles | `record()` yes |
| `float.hpp` | FTZ/DAZ, FP exception flags and traps, tolerance compare, saturating float→int | yes |
| `status.hpp` | `Status` (`[[nodiscard]]` enum) and `Result<T>`, used instead of exceptions | yes |
| `assert.hpp` | `COMMON_ASSERT`: never throws; calls a handler that must not allocate, then aborts | yes |
| `thread.hpp` | `RtThread` with explicit policy/priority/affinity, `CpuSet`, current-thread setters | init phase |
| `memory.hpp` | `mlockall`, glibc malloc tuning, stack pre-faulting | init phase |
| `spin.hpp` | `cpu_relax()` pause hint for spin loops | yes |

`#include "common/common.hpp"` pulls in everything.

## Real-time rules this library follows (and you should too)

1. **Two phases.** Initialisation may allocate, block and fail. After it ends, the
   real-time phase only uses the operations marked RT-safe above.
2. **No heap on the RT path.** Containers have compile-time capacity. Queues and pools embed
   their storage, so give them static storage duration, not stack.
3. **No blocking.** Threads talk through `SpscQueue`, `MpmcQueue` and `SeqLock`, never mutexes.
   A full queue returns `false`. The RT thread then drops the item or degrades; it never waits.
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

See [examples/rt_periodic_loop.cpp](examples/rt_periodic_loop.cpp) for the whole pattern: a
1 kHz RT loop that measures its own wake-up latency and hands data to a non-RT thread.

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

GCC/Clang warnings and `.clang-tidy` only *approximate* the rule set. Compliance must be
confirmed with a qualified checker, such as Helix QAC, Parasoft C++test, PC-lint Plus or
Polyspace.

Documented deviations (each is also marked in the code):

| Rule | Where | Rationale |
|---|---|---|
| 19.0.2 function-like macros | `COMMON_ASSERT` | Needs call-site file/line and the expression text |
| 21.6.3 advanced memory management | `StaticVector`, `ObjectPool` | Placement new into owned, aligned inline storage; this is what removes heap use |
| 8.2.5 `reinterpret_cast` | `StaticVector`, `ObjectPool` | Access to objects created in raw storage (with `std::launder`) and an address range check |
| 10.4.1 `asm` | `cpu_relax`, FPCR access (AArch64 only) | No intrinsic exists |

Tests and examples use `<cstdio>` and `std::thread` and are outside the compliance scope.
