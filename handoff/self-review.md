# Own review of patterns-spec.md (to merge with the reviewers' findings)

S1 TaskExecutor::stop() vs a concurrent submit(): a submit that passed the "stopping" check can push
   after stop() drained the queue, leaving the task kQueued forever (its submitter waits forever, its
   destructor asserts). Fix: an in-flight submit counter; submit increments it, checks the flag,
   pushes, decrements; stop() sets the flag, waits (non-RT, bounded sleep loop) until in-flight is 0,
   then joins and drains. seq_cst on the flag/counter handshake (Dekker-style).
S2 ActiveObject: same race. Stronger contract: a post() that succeeded always runs before stop()
   returns. Thread exits when stopping && in-flight == 0 && queue empty (in that order, seq_cst).
S3 Root type name "Object" can collide with a user type named Object (same_as compares names):
   name it "common::Object".
S4 UnitManager operator[] and find(): const and non-const overloads (a const manager hands out
   const Unit&).
S5 TaskScheduler::add() clears the task's trigger and cancel flags (stale requests from before).
S6 ActiveObject::post(): return Status (kResourceLimit: full or too contended; kNotRunning: stop()
   requested) like TaskExecutor::submit(), instead of bool. (Check the API reviewer's view.)
S7 wait_until_finished(): the loop bound must be explicit (number of polls computed from the
   deadline, saturating), not only the deadline test.
S8 TaskScheduler: run() of a task may call trigger() (any task) but not add/remove/run_once/start/
   suspend/resume of its scheduler (asserted). TaskExecutor: a job may submit() other jobs.
S9 ~TaskExecutor(): @pre workers stopped; cancels the jobs still queued (as stop() does), so no task
   is left kQueued (declare tasks before the executor). ~ActiveObject(): destroys queued commands.
S10 ObjectType depth/same_as: is_a walks up depth() - ancestor.depth() parents and compares once;
   this needs consistent depths, which construction guarantees (a duplicate in another module has the
   same depth).
