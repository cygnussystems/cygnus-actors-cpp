# Framework Invariants

Load-bearing properties of the threading model. Each one is stated so it can be
falsified, names the code that enforces it, and gives the question a reviewer
should ask when a diff touches it.

These are not style rules. Breaking one of these reintroduces a class of bug that
is intermittent, load-dependent, and survives testing — the kind that shows up in
production and nowhere else.

**Files governed by this document:** `source/actor.cpp`, `source/system.cpp`,
`include/cas/actor.h`, `include/cas/system.h`, `include/cas/actor_ref_impl.h`.
Any diff touching these gets the invariant questions asked explicitly.

---

## INVARIANT 1 — One thread per actor at a time

> Exactly one thread may execute an actor's handlers at any instant.

This is the core promise of the actor model. It is what lets users write handlers
with no locks, which is what the documentation teaches.

**Enforced by:**
- `actor::dispatch_message()` is **private** (`include/cas/actor.h`). The set of
  callers is closed and greppable.
- All message delivery funnels through `actor::process_next_message()`, run by
  the actor's single executor: its pool worker (via the ready queue), a fast
  actor's dedicated thread, or, for an `inline_actor`, the sender's thread -
  serialised by its mutex (`ThreadSafe=true`) or by the single-sender contract
  (`false`); re-entrant sends are queued and handled after the current handler.
  The calling thread executes an actor only when no executor exists:
  `stop_actor()` before `start()`, or after `wait_for_shutdown()` has joined
  every thread.
- Call requests are a priority lane inside the same `detail::mailbox`, drained by
  that same function — they do **not** get a separate thread.
- `stop_actor()`'s hooks run on the executor (`system::service_stop()`), not on
  the caller; `m_stop_phase` decides whether `stop_actor()` or system shutdown
  owns them.
- A runtime-created actor's mailbox stays unscheduled until its `on_start()`
  returns.
- A fast actor that blocks when idle (the ZeroMQ relay: `wait_for_work()`)
  still runs everything on its one dedicated thread; other threads only call
  its `wake()` (from `system::schedule()` when a push unblocks its mailbox,
  and from `stop_actor()`), which touches no actor state.
- Debug builds: `dispatch_guard` in `source/actor.cpp` aborts with a diagnostic if
  two threads enter dispatch for one actor.

**Reviewer questions:**
- Does this diff add a caller of `dispatch_message()`, or a `friend` declaration
  granting access to it?
- Does it introduce a thread that can reach an actor outside its owning worker?
- Does it make a call, timer, or relay path dispatch "directly" for latency?

**Test:** `tests/11_concurrency/test_call_serialization.cpp`,
`tests/07_dynamic_removal/test_stop_semantics.cpp` ("stop_actor runs hooks on
the actor's thread, after the running handler"),
`tests/11_concurrency/test_scheduler.cpp` ("A message cannot be handled while
the actor's on_start() is running").

**History:** violated between the introduction of the dedicated ask thread pool
and its removal. Call handlers ran on 4 shared threads while regular handlers ran
on the worker pool, so any actor with both had a silent data race. The fix was a
return to the original design. Do not reintroduce a separate ask thread.
Also violated, and fixed after 1.0.0: `stop_actor()` ran `on_shutdown()` /
`on_stop()` on the caller's thread, concurrently with the actor's handlers; and
an actor created at runtime could handle messages while its `on_start()` was
still running.

---

## INVARIANT 2 — No framework lock is held while user code runs

> No mutex owned by the framework may be held across a call into a user handler.

**Enforced by:** the worker loop (`system::worker_thread()` / `run_actor()`) takes
no lock at all: it pops ready actors from its lock-free ready queue. A worker's
mutex is used only to park and be woken, never around a handler.
`system::end_watches()` collects the watchers under `m_watchers_mutex` and sends
`termination_msg` after releasing it; `register_actor()` calls `on_start()` after
releasing the list locks; `service_stop()` runs the stop hooks with no lock held. `system::shutdown()` and `wait_for_shutdown()` work on snapshots of
the actor lists (`snapshot_pooled_actors()` / `snapshot_fast_actors()`) taken
under the list mutexes and used after releasing them: enqueueing
`system_shutdown` can run user code on the caller's thread (an `inline_actor`'s
`on_shutdown()`, queue-threshold and dead-letter callbacks), and
`wait_for_shutdown()` calls every pooled actor's `on_stop()`.

**Test:** `tests/11_concurrency/test_shutdown_callbacks.cpp` (on_stop, inline
on_shutdown and a threshold callback each calling `actor_count()` during
shutdown). Before the fix all three deadlocked - on MSVC as a
`resource_deadlock_would_occur` exception from `std::mutex`, which the test
catches and reports.

**History:** the worker loop used to take `m_actors_mutex` - a single
system-wide mutex - on every iteration to copy its actor list. First it held it
across handlers too (removed before 1.0); later, even just taking it per
iteration capped concurrent throughput at ~2.7 M msg/s on 24 threads (the
ready-queue scheduler, adapted from CAF, reached ~17 M).

**Why:** `m_actors_mutex` is a single system-wide, non-recursive mutex. Holding it
across a handler means (a) every worker contends on one lock on every iteration,
bounding throughput regardless of thread count; (b) one slow handler stalls all
workers; (c) any handler calling `system::create()` re-enters `register_actor()`
and self-deadlocks.

**Reviewer questions:**
- Does any `lock_guard`/`unique_lock` scope in this diff contain a call to
  `process_next_message()`, `dispatch_message()`, `on_start()`, `on_stop()`,
  `on_shutdown()`, or a user callback?
- If a handler called back into the system from here, would it deadlock?

**Test:** `tests/11_concurrency/test_no_lock_during_handler.cpp`

---

## INVARIANT 3 — Messages from one sender are handled in send order

> If sending A happens-before sending B, and both go to the same actor, A is
> handled before B. Messages from one actor, or from one thread, are therefore
> handled in the order they were sent. Every message accepted by a running actor
> is handled at most once: if `stop_actor()` completes first (discard, drain
> timeout, or a push racing the stop) it is reported as a dead letter instead;
> after a system-shutdown drain timeout, leftovers are only listed in
> `system::get_shutdown_log()`.
>
> **Exception — `call()`:** a call is handled *next*, after the handler currently
> running, ahead of every ordinary message still queued — including messages
> its own sender sent earlier. Calls are handled in order among themselves.

There is **no** ordering between different senders; that would need a global
lock. `stateful_actor`'s and `stateful_fast_actor`'s selective receive reorders by
design.

**Enforced by:** `cas::detail::mailbox` (`include/cas/detail/mailbox.h`,
adapted from CAF). Producers push onto one intrusive lock-free stack; every push
is a CAS on a single atomic word, so pushes are totally ordered, and a push that
happens-before another precedes it in that order. The single consumer takes the
whole stack and reverses it into two FIFO caches — calls, and everything else —
and serves calls first. Before serving each ordinary message it checks for calls
pushed since (via the call count), so a call never waits behind a backlog that
was already taken off the stack.

**Why calls jump the queue:** `call()` is the RPC path, and one of its jobs is
finding out why an actor is slow. A call stuck behind the backlog it is calling
about would be useless. The cost is the one documented exception: a caller that
does `tell(x)` then `call(...)` may be answered before `x` is handled.

**Reviewer questions:**
- Does this diff add a queue, a priority class, or any path that delivers a
  message other than through `m_mailbox.pop()` (push order, calls first)?
- Does it let an actor be consumed by more than one thread (which would
  reorder, as well as break INVARIANT 1)?
- Does new documentation promise ordering *between* senders?

**History:** early documentation promised FIFO, but the mailbox was a
`moodycamel::ConcurrentQueue` used without producer tokens and nothing enforced
or tested order; the docs were then corrected to "no guarantee". Calls lived in
a separate priority queue, as they still do (now inside the one mailbox). Calls
were briefly made strictly FIFO; that was reverted because a call must be
answerable while the actor is behind.

**Tests:** `tests/11_concurrency/test_message_ordering.cpp`,
`tests/14_mailbox/test_mailbox.cpp`

---

## INVARIANT 4 — Every declared metric has a writer

> A metric that is exposed must be updated by live code.

**Why:** `m_call_queue_high_water_mark` was exposed through
`call_queue_high_water_mark()` and included in `total_high_water_mark()`, but after
calls moved to a global queue nothing ever wrote it. It silently returned 0 while
appearing to be a real measurement. A metric that lies is worse than one that is
absent, because it is trusted. Later audit (issue 008): `dropped_to_invalid` had
no writer, the queue-threshold warning fired once per actor lifetime rather than
once per breach, `total_high_water_mark()` summed two unrelated maxima, and the
`queue_threshold_warning` message type was never sent. All fixed; the message
type is deprecated.

**Tests:** `tests/11_concurrency/test_metrics_and_naming.cpp`,
`tests/11_concurrency/test_queue_threshold.cpp`

**Reviewer questions:**
- If this diff makes a code path dead, does any metric, accessor, or config field
  now have no writer?
- Conversely, is a newly added metric actually written on every path that should
  update it?

---

## INVARIANT 5 — Degradation is observable in release builds

> Operational signals must not be compiled out.

**Why:** the queue threshold breach previously only printed under
`CAS_DEBUG_LOGGING`. In a release build, exceeding the threshold did nothing at
all — silent degradation under exactly the load where it matters.

**Enforced by:** `system::report_queue_threshold()` invokes a user callback,
modelled on `set_dead_letter_handler()`.

**Reviewer questions:**
- Is any new operational signal inside `#ifdef CAS_DEBUG_LOGGING`?
- Would an operator running a release build learn about this condition?

**Test:** `tests/11_concurrency/test_queue_threshold.cpp`


Handler exceptions (issue 014) follow the same rule: caught in `actor::dispatch_message()` and the stop hooks, counted (`system::handler_exception_count()`), passed to `set_handler_exception_handler()`, and written to stderr when no callback is set - never silently dropped, never fatal. Test: `tests/11_concurrency/test_handler_exceptions.cpp`.
---

## INVARIANT 6 — `on_start()` runs exactly once, and cannot block

> Every actor's `on_start()` is called exactly once before it processes any
> message, and nothing in `on_start()` may block on the message system.
> (An actor stopped with `stop_actor()` before `start()` never runs
> `on_start()`; its `on_shutdown()`/`on_stop()` still run.)

**Enforced by:**
- `system::start()` starts actors registered before it ran; `system::register_actor()`
  starts actors created after it. `m_running` decides which, so neither double-starts.
- All three call sites — `start()`, `register_actor()`,
  `fast_actor::run_dedicated_thread()` — go through `actor::run_on_start()`, which
  runs `on_start()` only if the actor's `m_started` flag was not yet set, inside an
  `on_start_scope` (actor context + `m_in_on_start`).
- `system::start()` after a shutdown without `reset()` throws `std::logic_error`
  (it would have re-run `on_start()` on stopped actors).
- `actor_ref::call()` throws `on_start_violation` when the caller is initialising.

**Why blocking is fatal here:** at startup no worker thread has been launched yet,
so a `call()` from `on_start()` waits for a reply nobody can produce. It hangs
rather than fails, with no diagnostic.

**Why `tell()` and `create()` are allowed:** both are non-blocking. `tell()`
queues; `create()` registers. Both are used throughout the documentation
(the supervisor pattern in `README.md`, request/response in
`doc/120_best_practices.md`) and must keep working.

**Reviewer questions:**
- Does this diff add a call site for `on_start()`? If so, does it use
  `on_start_scope`?
- Does it introduce a new blocking operation reachable from `on_start()`? If so,
  it needs the same `is_initialising()` check that `call()` has.
- Does it hold a lock across `on_start()`? That is INVARIANT 2 — `on_start()` may
  call `system::create()`.

**History:** `on_start()` was originally called only from `start()`, so actors
created at runtime were registered but never started — their handlers never
registered, and every message to them fell through to `on_unhandled_message()`
silently. The documented supervisor-restart pattern was broken by this.
Separately, `start()` held `m_actors_mutex` across `on_start()`, so an actor
creating another actor during initialisation self-deadlocked.

**Tests:** `tests/11_concurrency/test_runtime_actor_start.cpp`,
`tests/11_concurrency/test_on_start_contract.cpp`

---

## INVARIANT 7 — User code never throws out of a framework thread

> An exception from user code running on a framework thread (pool worker,
> fast actor thread, timer thread) is caught and reported; it never unwinds
> out of the thread, which would call `std::terminate` and kill every actor.

Where user code runs, and what happens when it throws:

| User code | Runs on | Outcome |
|-----------|---------|---------|
| `tell()` handler, `on_unhandled_message()` | executor | caught in `dispatch_message`, reported, `on_handler_exception()` decides resume/stop |
| `on_handler_exception()` | executor | caught; actor stopped; reported |
| `call()` handler | executor | stored in the promise; the caller rethrows |
| `inline_actor` handler | sender's thread | propagates to the sender's `tell()`/`call()` (by design) |
| `inline_actor` handler, reached by the framework (timer tick, `termination_msg` from a stop, relay routing / error fan-out) | timer thread / stopping actor's executor / relay thread | caught at the framework's send, reported (source `handler`, the inline actor's name); the framework carries on |
| `on_shutdown()`, `on_stop()` | executor / `wait_for_shutdown()` caller | `run_stop_hook`: reported, the stop completes |
| dead-letter, queue-threshold callbacks | whichever thread dropped / sent (incl. timer thread, workers) | caught, reported (source `dead_letter_handler` / `queue_threshold_handler`) |
| handler-exception callback | as above | caught; falls back to stderr |
| constructor (in `create()`) | caller | propagates out of `create()`; nothing registered |
| `on_start()` inside `create()` (running system) | caller | propagates out of `create()`; the actor is discarded (`discard_unstarted_actor`) |
| `on_start()` inside `start()` (pooled, created before start) | caller of `start()` | propagates out of `start()`; **treat as fatal** - shut down and exit |
| `on_start()` of a `fast_actor` created before `start()` | its dedicated thread | **reported, then `std::terminate()`** - no one can receive it |

The two fatal rows are deliberate: a failed `on_start()` is a programming error,
and recovering would cost complexity everywhere for no real benefit. They are
documented as such (a warning, not a safety net).

**Cost:** the guards are `try` blocks, which with table-based exception handling
(MSVC x64, GCC, Clang) cost nothing unless an exception is thrown. Do not add
guards that cost on the normal path (e.g. per-message allocation, locks, flags
checked per message) - document a warning instead.

**Enforced by:** `actor::dispatch_message()`, `actor::handle_handler_exception()`,
`actor::run_stop_hook()`, `system::report_queue_threshold()`,
`system::report_dead_letter()`, `system::report_invalid_target()`,
`system::report_handler_exception()`, `system::register_actor()`
(`discard_unstarted_actor`), `fast_actor::run_dedicated_thread()`.

**Reviewer questions:**
- Does this diff add a place where user code is called (a hook, a callback, a
  handler form)? Which thread runs it, and what happens if it throws? Is there a
  row for it in the table above and in `tests/15_failure_modes/`?
- Does the guard cost anything when nothing throws?

**History:** until issue 014 (2026-09-28) nothing was caught: one throwing
handler terminated the process. It went unnoticed because no test ever made a
`tell()` handler throw - the only throwing test was a `call()` handler, the one
path that already handled it.

**Tests:** `tests/15_failure_modes/test_failure_modes.cpp` (the matrix: every
row above, for pooled, fast, stateful and stateful fast actors, plus inline
actors), `tests/11_concurrency/test_handler_exceptions.cpp`, and the
`test_fatal_on_start` ctest (`tests/15_failure_modes/fatal_probe.cpp`, expected
to terminate with the report).

---

## Test hygiene — why every test declares a guard

The actor system is a process-wide singleton, so a test that leaves it running
corrupts every test after it: `start()` returns early when already running, so
`on_start()` never runs and no handlers register. One real failure then produces a
cascade of fake ones, and the only trustworthy line in the output is the first.

Catch2's `REQUIRE` throws on failure, so cleanup written at the *end* of a test
body is skipped exactly when it is most needed. Cleanup must therefore be RAII:

```cpp
TEST_CASE("...") {
    CAS_TEST_GUARD();     // resets the system on scope exit, including on throw
    ...
}

TEST_CASE("...") {
    CAS_CONFIG_GUARD();   // as above, plus restores default system_config and
    ...                   // clears global handlers - use when the test calls
}                         // configure() or set_*_handler()
```

Both guards are `noexcept` in the destructor: they run during unwinding, where
throwing would call `std::terminate`.

**Reviewer question:** does every new `TEST_CASE` that calls `system::start()`
declare a guard on its first line?

This is checked automatically rather than by hand. `tests/12_isolation/` builds a
second binary, `isolation_probe`, which **is expected to fail**: it contains one
deliberately-failing test followed by four healthy ones. The CTest target
`test_isolation` runs it and asserts the failure count is exactly 1. More than
that means a failing test corrupted the singleton and took its successors with
it.

The healthy tests assert `is_running() == false` and `actor_count() == 0` on
entry. Without those explicit preconditions the probe was insensitive — each
cycle creates a fresh actor whose `set_name()` overwrites the registry entry, so
a cycle could pass even with the system left running. Verified both ways:
removing a guard from the probe makes `test_isolation` fail with a diagnostic
naming the cause.

---

## Defense in depth

Tests alone cannot prove the absence of a data race — races are probabilistic and
a stress test that fails 95% of the time can pass on a quiet CI machine. The
layers, in order of reliability:

1. **Compile-time** — `dispatch_message()` is private. Reintroducing INVARIANT 1
   requires editing a `friend` declaration in a header, which is a visible,
   reviewable act rather than a plausible-looking change in a `.cpp`.
2. **Debug assertion** — `dispatch_guard` aborts on the first concurrent dispatch,
   whether or not the race manifests. Verified: reintroducing the original defect
   trips it immediately.
3. **Regression tests** — `tests/11_concurrency/`, including a 2N counter test on a
   deliberately non-atomic `int`.
4. **Sanitizers** — recommended CI addition. ThreadSanitizer (Linux/Clang or GCC)
   detects the unsynchronized access without needing the race to occur. This is
   the strongest available check and would have caught the original defect on the
   first run.

Recommended CI: a debug build (assertions live), a TSan build, and the stress
tests run repeatedly (`--repeat 50`) on varying core counts.
