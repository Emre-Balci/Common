# Rules for small tasks

You are a small local model with a short context. Work in the current worktree only.

- Do exactly the task in the handoff's Goal. Change no other file. If the task needs a design decision, a
  new dependency or a file you were not told about, stop and say so in your final message instead.
- Library code (`include/`, `src/`) is C++17: no `new`, `std::vector`, `std::string`, `std::function` or
  exceptions on the real-time path, everything `noexcept`, braces on every branch, fixed-width types,
  4-space indent, at most 120 columns, `lower_case` functions, `CamelCase` types, `member_` fields.
- Build and test, only when the task changes code:
  `cmake --preset gcc-debug && cmake --build --preset gcc-debug && ctest --preset gcc-debug`
  (building on `/mnt/<drive>` is slow, but your worktree is on the Linux file system).
- Commit on the current branch with a short message. Never push, merge, or delete a branch.
- Do not edit the handoff file: the script records the run in it.
