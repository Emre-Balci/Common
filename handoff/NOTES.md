# Handoff notes: branch `feature/common-base-and-patterns`

For the next session (Claude Code or a person) that continues this work. Delete the whole `handoff/`
directory before the branch is merged.

## The request (2026-10-26)

> Create a new branch. There should be two libraries as the output: common-base and common. The first
> should include what is now here. The other should include some other classes too which I want you to
> add. Start by adding patterns such as Task pattern, object type pattern, Unit pattern etc. which could
> be re-used when creating complex apps.

The user's answers to the clarifying questions:
- **Unit** = lifecycle unit: initialize/start/stop/shutdown hooks, dependencies between units, and a
  manager that brings them up in dependency order, down in reverse, and rolls back on failure.
- **Task** = both kinds, sharing one `Task` interface: periodic and triggered real-time tasks run by a
  cyclic `TaskScheduler`, and one-shot jobs run by a `TaskExecutor` (fixed pool of worker threads).
- **Object type** = Type Object pattern plus run-time type identity without RTTI (`ObjectType`,
  `object_cast`, a registry with factories).
- **Extras** for this first batch: Observer/signal, state machine, heap-free callable
  (`InplaceFunction`), Active Object.

Later instruction: if the session limits reach 95% before the work is done, commit the work and write
a note for another session (this file).

## State

Done and verified:
- Commit `701942d` "Split the project into the libraries common-base and common" (local, not
  pushed): the split below, with every component of `common` as a placeholder stub.
- The multi-agent workflow failed when the session limit was hit; the next session implemented every
  component itself instead: commits `f9b2be1` (InplaceFunction, Signal, object types, StateMachine),
  `840b662` (Task, TaskScheduler, TaskExecutor), `124da01` (ActiveObject, Unit, UnitManager). Each
  builds warning-free and passes on gcc-debug, gcc-release, asan (and tsan for the concurrent ones);
  each source compiles warning-free with MSVC 19.32 and the NDK (msvc-check.sh / ndk-check.sh).
  Deviation from the spec: TaskExecutor::stop(kCancel) does not cancel() running jobs (that could race
  with the owner destroying a job that just finished); it cancels the queued ones only. Added
  TaskExecutor::stopping() and ActiveObject::stopping().
- Commit `162381c`: tests/app/test_application.cpp (integration) and examples/unit_application.cpp.
- Commit `ac33e3c`: README "common components" section, deviation rows, CLAUDE.md "Rules learned".
  The full matrix passed on this code (common_tests on every preset that runs tests; msvc-asan run
  with -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded in build/msvc-asan-static).
- Last step in progress: a final adversarial review (four lenses, each finding verified by a
  skeptic) plus an independent concurrency review. Apply the confirmed findings, re-run the affected
  presets (and the matrix if code changed), commit, then delete handoff/ in the last commit.
- The library split. `common-base` (alias `common::base`, files `libcommon-base.a`/`common-base.lib`)
  is all the code that existed before, unchanged. `common` (alias `common::common`) holds the new
  groups `functional`, `object`, `event`, `state`, `task`, `app` and links `common-base` publicly.
  Umbrella headers `common/base.hpp` (base only) and `common/common.hpp` (both). Tests are split into
  `common_base_tests` (links only `common::base`) and `common_tests`; the CTest
  `common_base_layering` (`tests/check_layering.cmake`) fails if a file of `common-base` includes a
  header of a `common` group. README (intro table, build and usage sections) and CLAUDE.md (Layout)
  describe it. `rt_periodic_loop` links `common::base` only; `unit_application` is the new example.
- With the skeleton (empty stubs for every new file), every preset built warning-free: gcc-debug,
  gcc-release, clang-release, asan, tsan, portable, android-arm64, android-x86_64, msvc-debug,
  msvc-release, msvc-asan. The only test failures are machine-specific and identical on `main`
  (see "Known failures").

In progress when this note was written: the nine components and the integration test/example, built
by a multi-agent workflow from the spec. Each component went implement → two independent reviews →
fix → re-review (→ second fix). An agent copies a component's files back into the working tree only
after they build warning-free and pass on gcc-debug/gcc-release/asan(/tsan) and compile with MSVC and
the NDK. **To see what is finished, look at the files: one that still contains
`STUB: not implemented yet.` is not done.** `git log` shows what is committed.

| Component | Files | Depends on |
|---|---|---|
| inplace_function | `include/common/functional/inplace_function.hpp`, `tests/functional/test_inplace_function.cpp` | — |
| object | `include/common/object/object_type.hpp`, `object_type_registry.hpp`, `tests/object/test_object_type*.cpp` | — |
| state_machine | `include/common/state/state_machine.hpp`, `tests/state/test_state_machine.cpp` | — |
| task | `include/common/task/task.hpp`, `src/task/task.cpp`, `tests/task/test_task.cpp` | — |
| unit | `include/common/app/unit*.hpp`, `src/app/unit*.cpp`, `tests/app/test_unit*.cpp` | — |
| signal | `include/common/event/signal.hpp`, `tests/event/test_signal.cpp` | inplace_function |
| active_object | `include/common/task/active_object.hpp`, `tests/task/test_active_object.cpp` | inplace_function, task |
| task_scheduler | `include/common/task/task_scheduler.hpp`, `src/task/task_scheduler.cpp`, `tests/task/test_task_scheduler.cpp` | task |
| task_executor | `include/common/task/task_executor.hpp`, `src/task/task_executor.cpp`, `tests/task/test_task_executor.cpp` | task |
| integration | `tests/app/test_application.cpp`, `examples/unit_application.cpp` | all of the above |

## Design

`handoff/patterns-spec.md` is the contract (v2). Four independent reviewers (library rules/MISRA,
concurrency, API for automotive/trading apps, portability with probes compiled on GCC 9.4, Clang 10,
NDK r29 and MSVC 19.32) attacked v1; v2 folds their findings in. The decisions most likely to be
questioned, and why:
- Interface bases (`Task`, `Unit`, `Object`) have protected non-virtual destructors; leaf classes are
  `final`; intermediate classes declare protected destructor/copy/move and **no friends** (a friend
  makes GCC's -Wnon-virtual-dtor fail). Runners reach a task's internals through a passkey
  (`detail::TaskKey`).
- Type identity: same object, or (both ids non-zero and equal ids and names). A name alone never
  identifies a type, otherwise `object_cast` could match an unrelated class of the same name (UB).
- `TaskExecutor`/`ActiveObject`: claim the task first (CAS), then write; an in-flight counter with
  seq_cst handshakes so that `stop()` never strands accepted work; `stop(StopMode::kDrain|kCancel)`;
  restart after stop allowed; heartbeats parked (`beat(TimePoint::max())`) while not running.
- `TaskScheduler` owns its thread (or is driven with `align()` + `run_once()`); exact activation
  arithmetic (latest overdue grid point runs late, the skipped ones count as missed); priority never
  preempts; `Task::trigger/suspend/resume/cancel` are wait-free requests applied by the runner.
- `InplaceFunction` callables must return exactly `R` (a `[[nodiscard]] Status` cannot be dropped
  silently); the operations table is a member of a class template (a static data member template is
  MSVC C4268); default alignment fixed at 16 (MSVC's `max_align_t` is 8).
- `TaskStatistics` is a trivial aggregate (published through `SeqLock`; default member initialisers
  trip GCC 9's -Wclass-memaccess).
- `Signal::notify()` (not `emit`: Qt macro).
- Status codes: `kAlreadyExists` duplicate registration or task already in a runner; `kAlreadyRunning`
  start of something running; `kNotRunning` stop of something stopped / submit while stopping;
  `kInvalidArgument` bad argument or call in the wrong state.
- The kType idiom hides the base's `kType` in each class: if a project's MISRA checker applies Rule
  6.4.1 to class scopes, the project records a deviation (the library itself hides nothing).

## Building and verifying on this machine

WSL1 (Ubuntu 20.04, kernel 4.4.0-18362-Microsoft): GCC 9.4, Clang 10, CMake 4.4, Ninja 1.10; MSVC 19.32
through `vcvars64.bat` of VS 2022 Enterprise; Android NDK r29 at `/root/Android/Sdk/ndk/29.0.14206865`.
About 3 GB of RAM are free: keep builds to 2 jobs each and at most 4 at a time. Building on the Linux
file system (a copy under /tmp) is far faster than on /mnt/c.

Scripts in `handoff/bin` (set `export SCRATCH=<a scratch directory>` first):
- `sync-to-work.sh <name>` / `build.sh <name> <preset> [filter] [target]` / `sync-back.sh <name> <files>`:
  private copies of the tree for parallel work, builds with the project's presets, atomic copy-back.
- `msvc-check.sh <name> <cpp files>`, `ndk-check.sh <name> <cpp files>`: compile-only checks with the
  project's MSVC and NDK flags.
- `matrix-linux.sh`, `matrix-android.sh`, `matrix-msvc.sh`: the full preset matrix on the repository
  itself, one summary line per preset.

Known failures (machine-specific, identical on `main`; do not chase them):
- every Linux preset: the 7 `serial_port_*` tests (WSL1 pseudo-terminals lack TIOCEXCL);
- android-x86_64 run on this host: 12 tests (the 7 serial ones, the two assert-abort tests,
  `file_system_operations`, `whole_file_write_file_atomically`,
  `whole_file_atomic_replacement_is_never_torn`): WSL1's old kernel and bionic;
- msvc-asan: the test programs do not start (0xc0000142): the dynamic ASan runtime of VS 17.2 fails on
  this Windows 10 1909; earlier sessions ran them with the static runtime
  (`-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`);
- msvc-release: `logger_doubles_match_printf` (printf of Windows 10 1909 rounds exact ties up);
- `periodic_timer_reports_overrun` (1 ms margin) can fail when the machine is loaded (seen under tsan
  and msvc-debug while other builds ran); rerun it alone.

## Remaining steps

1. Finish any component that is still a stub or was not reviewed (follow the spec; implement, then an
   independent review, then fix).
2. Integrate: run the three matrix scripts on the repository; fix what fails (only the known failures
   above may remain). CLAUDE.md requires gcc-release, clang-release, asan, tsan, portable, the MSVC
   presets and both Android presets warning-free.
3. A final cross-component review: consistent names, Status codes, documentation tone, examples.
4. Documentation: README "## common components" section with one table per new group (the intro
   already links `#common-components`); README MISRA deviation rows for new deviations (InplaceFunction
   placement new 21.6.3 and reinterpret_cast 8.2.5, and any others the components report) and a note
   on the kType idiom; the `unit_application` example in the README; CLAUDE.md conventions for the
   patterns (the portability rules of spec §1).
5. Commits on this branch, in the repository's style (`git log` shows it: a subject line, an
   explanation, what was verified on which platform, and which tests fail because of the machine).
   Identity and trailer:
   `git -c user.name="Emre BALCI" -c user.email="emre.balci@tubitak.gov.tr" commit ...`, message ending
   with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Do not push unless the user asks.
6. Delete `handoff/` in the last commit, and report to the user.
