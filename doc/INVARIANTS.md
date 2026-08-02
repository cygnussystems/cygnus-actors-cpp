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
- All message delivery funnels through `actor::process_next_message()`, which runs
  only on the actor's owning worker thread.
- Ask requests go into the actor's own `m_ask_queue` and are drained by that same
  function — they do **not** get a separate thread.
- Debug builds: `dispatch_guard` in `source/actor.cpp` aborts with a diagnostic if
  two threads enter dispatch for one actor.

**Reviewer questions:**
- Does this diff add a caller of `dispatch_message()`, or a `friend` declaration
  granting access to it?
- Does it introduce a thread that can reach an actor outside its owning worker?
- Does it make an ask, timer, or relay path dispatch "directly" for latency?

**History:** violated between the introduction of the dedicated ask thread pool
and its removal. Ask handlers ran on 4 shared threads while regular handlers ran
on the worker pool, so any actor with both had a silent data race. The fix was a
return to the original design. Do not reintroduce a separate ask thread.

---

## INVARIANT 2 — No framework lock is held while user code runs

> No mutex owned by the framework may be held across a call into a user handler.

**Enforced by:** `system::worker_thread()` copies its per-thread actor list under
`m_actors_mutex`, releases the lock, then processes. `system::notify_watchers_internal()`
copies the watcher set under lock and sends outside it.

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

## INVARIANT 3 — Ordering guarantees are stated accurately

> The documentation promises exactly what the mailbox delivers, and no more.

The mailbox is `moodycamel::ConcurrentQueue` enqueued **without** a `ProducerToken`.
That does not provide FIFO ordering across producers. What *is* guaranteed is
exactly-once delivery: no message is lost or duplicated.

**Reviewer questions:**
- Does this diff change the mailbox type or enqueue path? If so, does
  `doc/40_message_passing.md` still describe what the code actually does?
- Does any new documentation use the word "guaranteed" about ordering?

**If you want real FIFO:** switch the mailbox to an MPSC queue. Each actor has
exactly one consumer, so MPMC is over-general — an MPSC queue would be both faster
and ordered. Until that happens, the docs must not promise ordering.

**Test:** `tests/11_concurrency/test_message_ordering.cpp`

---

## INVARIANT 4 — Every declared metric has a writer

> A metric that is exposed must be updated by live code.

**Why:** `m_ask_queue_high_water_mark` was exposed through
`ask_queue_high_water_mark()` and included in `total_high_water_mark()`, but after
asks moved to a global queue nothing ever wrote it. It silently returned 0 while
appearing to be a real measurement. A metric that lies is worse than one that is
absent, because it is trusted.

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

---

## INVARIANT 6 — `on_start()` runs exactly once, and cannot block

> Every actor's `on_start()` is called exactly once before it processes any
> message, and nothing in `on_start()` may block on the message system.

**Enforced by:**
- `system::start()` starts actors registered before it ran; `system::register_actor()`
  starts actors created after it. `m_running` decides which, so neither double-starts.
- `on_start_scope` (in `include/cas/actor.h`) sets the actor context and the
  `m_in_on_start` flag at all three call sites — `start()`, `register_actor()`, and
  `fast_actor::run_dedicated_thread()`.
- `actor_ref::ask()` throws `on_start_violation` when the caller is initialising.

**Why blocking is fatal here:** at startup no worker thread has been launched yet,
so an `ask()` from `on_start()` waits for a reply nobody can produce. It hangs
rather than fails, with no diagnostic.

**Why `tell()` and `create()` are allowed:** both are non-blocking. `tell()`
queues; `create()` registers. Both are used throughout the documentation
(the supervisor pattern in `README.md`, request/response in
`doc/120_best_practices.md`) and must keep working.

**Reviewer questions:**
- Does this diff add a call site for `on_start()`? If so, does it use
  `on_start_scope`?
- Does it introduce a new blocking operation reachable from `on_start()`? If so,
  it needs the same `is_initialising()` check that `ask()` has.
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
