# Changelog

All notable changes to the Cygnus Actor Framework are documented here.

This project follows [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Deprecated

- **`queue_threshold_warning`**: the framework never sent it. Use
  `system::set_queue_threshold_handler()`.

- **`ask()` is renamed `call()`**, because it is a blocking function call
  into an actor (RPC), not a question. Renamed along with it:
  `ask_handler` -> `call_handler`, `ask_deadlock_error` ->
  `call_deadlock_error`, `ask_type_error` -> `call_type_error`,
  `ask_queue_high_water_mark()` -> `call_queue_high_water_mark()`, and the
  header `ask_message.h` -> `call_message.h`. Every old name still compiles
  and forwards to the new one, with a deprecation warning that names it.
  `dead_letter_stats::dropped_ask` keeps its name: a struct field cannot be
  aliased. The docs chapter, tutorial and tests are renamed to match
  (`doc/80_call.md`, `tutorials/06_call.cpp`, `tests/05_call/`).

### Added

- **`cas::stateful_fast_actor`**: selective receive on a dedicated thread, for
  state machines that are also latency-critical (e.g. an exchange session
  holding orders until logged in). Same API as `stateful_actor`; the
  filtering logic is shared between the two.
- **Messages from one sender are handled in send order** - now a guarantee
  (INVARIANT 3): if sending A happens-before sending B to the same actor, A
  is handled first. That covers one actor messaging another and one thread
  (including `main()`) messaging an actor. There is still no order between
  different senders. The one exception is `call()`: a call is handled next,
  after the running handler and ahead of every queued ordinary message -
  including ones its own sender sent earlier - so an actor that is behind can
  still answer (for example, a query about why it is slow). Calls are ordered
  among themselves. Earlier documentation
  first claimed FIFO without the implementation providing it, then said there
  was no ordering at all.
- **New actor mailbox** (`cas::detail::mailbox`), adapted from CAF's
  `lifo_inbox`/`default_mailbox` (BSD-3-Clause; see `THIRD_PARTY_NOTICES.md`):
  an intrusive lock-free stack that producers push to with one CAS, and that
  the single consumer takes whole and reverses into send order. It replaces
  the two `moodycamel::ConcurrentQueue`s per actor. It also carries idle and
  closed states that the scheduler work builds on. New stress tests in
  `tests/14_mailbox/`.
- `benchmarks/latency_benchmark.cpp`: round-trip latency percentiles (p50 to
  p99.9, max) for pooled and fast actors; and an 8-senders-to-8-receivers
  scenario in the throughput benchmark.
- `THIRD_PARTY_NOTICES.md`, covering CAF and moodycamel.

### Changed

- **The ZeroMQ relay is split into a relay and a transport** (internal,
  no behaviour change): routing, correlation, errors and status stay in
  `zeromq_relay_actor`; all ZeroMQ code moved to `detail::zeromq_transport`
  behind a transport interface (`detail::transport`), the first step
  towards pluggable transports (shared memory, Unix sockets). The interface
  is internal and may change within 1.x.

- **Pooled actors are scheduled from ready queues, as in CAF**, instead of
  workers scanning every actor pinned to them. Before, each worker took the
  single system-wide `m_actors_mutex` on every loop iteration to copy its
  actor list and then handled one message per actor, and every `tell()`
  locked a mutex and signalled a condition variable. Adding senders made
  the system slower: 8 actors sending to 8 others managed 2.7 M msg/s in
  total, less than one thread alone.

  Now an actor goes on its worker's ready queue only when a message finds it
  idle. It then handles up to `system_config::max_messages_per_turn`
  messages (new, default 64, 0 = unlimited) before other ready actors on
  that worker get a turn. A worker takes no lock on this path, and is
  signalled only when it has actually parked. The ready queue is an
  intrusive lock-free queue, FIFO across all producers.

  Measured on MSVC Release: 8 senders to 8 receivers 2.7 -> 17.2 M msg/s;
  one sender 4.5 -> 6.5 M msg/s; fast-actor round trip p99.9 8.0 -> 2.8 us.
- **Pooled workers spin briefly before parking**
  (`system_config::worker_idle_spin`, new, default 20 us, 0 = park at
  once). Work arriving within the spin is picked up without an OS wake-up.
  Pooled-to-pooled round trips drop from p50 1.9 / p99 15 / p99.9 24 us
  (1.0) to 0.7 / 1.7 / 3.7 us, on a par with `fast_actor`. The cost is CPU,
  roughly spin length x bursts of work per second on each busy worker
  (20 us x 1000 bursts/s = 2% of a core); idle workers do not spin, and a
  worker woken by its backstop timeout with nothing to do goes straight
  back to sleep.
- The benchmarks section of the README is re-measured, now on a Ryzen 9
  5900X desktop, adding latency percentiles and the concurrent-actor case.
  The one regression is recorded there: a single sender of messages with
  heap-allocating `std::string` members is ~15% slower than 1.0 on MSVC,
  while the same fields as `fixed_string` are ~2x faster. The likely cause
  is heap contention now that sender and receiver run concurrently; it has
  not yet been profiled.
- `call()` requests keep their priority (as in 1.0), now as a lane inside the
  single mailbox, and more strictly: a call pushed while the actor is working
  through a backlog it has already taken from the mailbox is still handled
  next, rather than after that backlog.
- Messages still queued when a stop completes (for example after a drain
  timeout) are now counted as dead letters rather than left unprocessed and
  uncounted.
- `message_base` is 8 bytes larger (the mailbox link), so some messages move
  up a pool size class.

### Fixed

- **`stop_actor()` on a stateful actor returned before its held messages were
  reported.** Held messages were dropped (as dead letters) only after the
  actor was marked stopped, so a caller waiting in `stop_actor()` could see
  the dead-letter count before they were counted. They are now dropped
  before the stop completes, like the rest of the mailbox.
- **An exception thrown by a message handler no longer terminates the
  process.** A `tell()` handler of a pooled, fast or stateful actor that threw
  unwound out of a framework thread, which calls `std::terminate`: one bad
  message took down every actor. The exception is now caught, counted
  (`system::handler_exception_count()`) and passed to the new
  `system::set_handler_exception_handler()` callback (to stderr when none is
  set), and the actor handles its next message. The new virtual
  `actor::on_handler_exception()` can return `exception_action::stop` to stop
  the actor instead (queued messages become dead letters, watchers get
  `termination_msg`). Exceptions from `on_shutdown()`/`on_stop()` are reported
  the same way and no longer abort the stop. `call()` and `inline_actor`
  handler exceptions still go to the caller/sender.
- **User callbacks can no longer take down a framework thread.** An
  exception from the dead-letter or queue-threshold callback - which run on
  whichever thread dropped or sent the message, including the timer thread
  and workers - is caught and reported (source `dead_letter_handler` /
  `queue_threshold_handler`) instead of terminating the process or surfacing
  from an unrelated `tell()`. A throwing handler-exception callback falls
  back to stderr.
- **A failed `on_start()` inside `create()` no longer leaves a half-created
  actor behind.** The exception still propagates to the caller; the actor is
  now discarded (it held every later shutdown for the full drain timeout).
  A `fast_actor` created before `start()` whose `on_start()` throws is
  reported before the process terminates, instead of terminating silently.
  New INVARIANT 7 and a failure-mode test matrix
  (`tests/15_failure_modes/`) cover every place user code runs.
- **`call()` deadlock detection is decided by thread, not by actor.** It
  compared the calling actor's worker with the target's, which was wrong
  both ways: a pooled actor calling an `inline_actor` that happened to share
  its worker number got a spurious `call_deadlock_error` (the call runs
  synchronously), while an inline actor's handler - running on its sender's
  worker - could call a pooled actor on that same busy worker and hang. Now
  the call is refused only when the calling thread is the one that has to
  run the target; calls to inline actors are always allowed.
- **An `inline_actor` can send to itself.** A handler telling its own inline
  actor (directly or through other inline actors) deadlocked on the
  thread-safe variant's mutex, and nested one handler inside another on the
  single-sender variant (a Debug-build abort). Such messages are now handled
  right after the current handler, in send order, before the outer `tell()`
  returns. A `call()` back into an inline actor that is mid-handler throws
  `call_deadlock_error`. A handler exception now reaches the sender with the
  actor left usable.
- **Metrics that did not measure what they said (INVARIANT 4):**
  - `dead_letter_stats::dropped_to_invalid` was never written. `tell()` and
    `call(op, timeout, ...)` on an invalid `actor_ref` now count there and
    reach the dead-letter handler (empty target name).
  - The queue-threshold handler fired once per actor lifetime. It now fires
    once per breach: it re-arms when the queue is back to half the
    threshold.
  - `total_high_water_mark()` returned the sum of two separate maxima; it
    is now the largest combined queue seen.
- **Small races and leaks:**
  - `actor::name()` wrote the default name lazily from whichever thread
    asked first (a data race with dead-letter reports from sender threads).
    The default name is assigned at creation, and `set_name()` publishes
    names so `name()` can be read from any thread. Renaming now releases
    the old registry name.
  - The registry holds weak references: it no longer keeps actors alive.
  - Actors that schedule many one-shot timers no longer track fired ids
    forever.
  - Timer messages carry a `message_id` (they had 0).
- **ZeroMQ relay:**
  - Inbound messages are drained, up to 256 per wake-up, instead of one per
    10 ms poll tick (about 100 msg/s).
  - All relays share one ZeroMQ context, so `inproc://` works between them.
  - `auto_reconnect` / `reconnect_delay_ms` are applied (ZeroMQ's reconnect
    interval); sockets get a bounded linger so teardown cannot hang.
  - Errors are also sent to the actor whose control message failed, so a
    bind/connect failure before any route exists is no longer lost.
  - A second relay no longer takes over the `"zeromq_relay"` registry name.
  - Unanswered correlation ids expire after the new
    `zeromq_config::request_timeout_ms` (default 30 s).
- **Timers no longer fire into a stopping actor during system shutdown.**
  `stop_actor()` cancelled an actor's timers, but system shutdown left them
  running until `wait_for_shutdown()`: after `on_shutdown()` the actor
  rejects messages, so every tick was counted as a dead letter (and passed
  to the dead-letter handler) on each clean shutdown. An actor's timers are
  now cancelled right after its `on_shutdown()`, including any it scheduled
  there. A stopping actor also can no longer schedule timers:
  `schedule_once()`/`schedule_periodic()` return `INVALID_TIMER_ID` (for
  example from a handler still draining after `on_shutdown()`). A tick
  already fired when the timer is cancelled can still arrive as a dead
  letter.
- **`stateful_actor` works, and is tested again.** Its tests had been
  excluded from the build since before 1.0, and it had defects:
  - a `call()` to it waited forever unless the current state happened to
    accept the framework's internal request type;
  - held messages were missing from `queue_size()` and the shutdown log, and
    were never reported as dead letters when the actor stopped;
  - it took two mutexes on every poll.
  Selective receive is now one lock-free, owner-thread component shared by
  `stateful_actor` and `stateful_fast_actor`. Calls and system shutdown
  bypass the filter, held messages keep their order and are counted, and
  stopping reports them as dead letters. The test file is back in the build,
  rewritten, and runs every case against both classes. `stateful_actor`'s
  API is unchanged; accept/reject must now be called from the actor's own
  thread (on_start or handlers), which is how it was documented.
- **The watch pattern could use a destroyed actor.** The watch table held
  raw actor pointers, keyed by the watched actor's address. A watcher
  destroyed before the actor it watched was dereferenced when that actor
  stopped (a crash, reproduced by the new tests). A watcher that had stopped
  but was still referenced was sent a `termination_msg` that became a
  spurious dead letter. `reset()` kept the table, so watches from one run
  could fire in the next. Entries for stopped watchers were never removed.
  Watches are now keyed by instance id and hold watchers weakly. Stopped or
  destroyed watchers are skipped, a stopping actor is removed from
  everything it watched, and `reset()` clears the table. The watch pattern
  had no unit tests; `tests/07_dynamic_removal/test_watch.cpp` adds seven.
- **User code that called back into the system during shutdown
  deadlocked.** `wait_for_shutdown()` ran every pooled actor's `on_stop()`
  while holding the system-wide actor-list mutex. `shutdown()` held the
  list mutexes while enqueueing `system_shutdown`, which can run user code
  on the calling thread: an `inline_actor`'s `on_shutdown()`, and the
  queue-threshold and dead-letter callbacks. Any of those calling back into
  the system - even `system::actor_count()` - re-took the same mutex, which is
  undefined behaviour: typically a hang, and on MSVC `std::mutex` detects it
  and throws, aborting the program. Both functions
  now work on snapshots of the actor lists and call nothing with a
  framework lock held (INVARIANT 2). Regression tests in
  `tests/11_concurrency/test_shutdown_callbacks.cpp`.
- The test runner no longer lets a crashing test raise a Windows dialog
  ("abort() has been called", error reporting, the Just-In-Time debugger
  prompt). CRT messages go to the console, and an unhandled crash prints its
  exception code and ends the run - on a desktop a dialog looked like the
  machine crashing, and on CI it would hang the job.
- **An actor created while the system was running could receive messages
  while its `on_start()` was still executing.** It was listed on its worker
  before `on_start()` ran on the creating thread, so a reply to something
  `on_start()` sent could be handled concurrently with it - two threads in
  one actor. Its mailbox now stays unscheduled until `on_start()` returns.
- **Every message to a fast actor woke pooled worker 0**, whose index fast
  actors carried by default: a mutex and a condition-variable signal per
  message on the latency-critical path. Fast actors are never scheduled on
  the pool now.
- Per-worker wake-up state was destroyed in `wait_for_shutdown()` while fast
  actors or other threads could still signal it. It now lives until
  `reset()`.
- **A `tell()` racing an actor's stop could be silently lost.** `tell()`
  checks that the actor is running and then enqueues; if the stop completed
  between the two, the message sat in a mailbox nobody would read, and was
  never reported as a dead letter. The mailbox is now closed atomically when
  a stop completes, so such a push fails and is counted as a dead letter.
- Message dispatch no longer uses `typeid` and `dynamic_cast` to recognise
  framework messages (shutdown, call); a private virtual identifies them.

- **The message pool could hand one block to two owners.** Its free list was
  a hand-rolled lock-free stack whose pop - load head, read `head->next`, CAS
  head - was open to ABA: another thread could pop that block and the next
  and push the first back between the read and the CAS, so the CAS installed
  a block already in use. With two threads that both allocate and free, a
  stress loop crashed within ~80 ms every time; a single allocating thread -
  the shape of every existing test - cannot trigger it. The shared free list
  is now a `moodycamel::ConcurrentQueue` (already vendored for the
  mailboxes), fronted by a per-thread cache of up to 32 blocks per size class
  that refills and flushes in batches of 16. Single-thread alloc+free costs
  what it did (~11 ns, against ~33 ns for the system allocator), same-thread
  churn under contention is 3-4x faster, and end-to-end throughput is
  unchanged to slightly better once a size class is warm; the first
  workload in a size class takes more pool misses while caches fill, which
  `prewarm()` avoids. New tests in `tests/13_message_pool/`.
- **Over-aligned messages were misaligned.** `message_base` only defined
  `operator new(size_t)`, which a new-expression falls back to for an
  `alignas(64)` type, returning a pointer aligned only to the pool header.
  Such messages now bypass the pool through aligned `operator new`/`delete`.
- **On MSVC, every pooled message was 8 bytes off a 16-byte boundary.** The
  size header was padded to `alignof(std::max_align_t)`, which MSVC defines
  as 8, while `operator new` guarantees 16 and the language treats
  `alignas(16)` types as not over-aligned. The header is now padded to the
  new `message_pool::POOLED_ALIGNMENT` (`__STDCPP_DEFAULT_NEW_ALIGNMENT__`).

### Changed

- **`message_pool::HEADER_OVERHEAD` is now 16 bytes on MSVC x64** (was 8),
  matching GCC and Clang, as a consequence of the alignment fix above. On
  MSVC a message within 8 bytes of a size-class boundary moves up one class,
  and `max_payload_for_class()` returns 8 less - a `static_assert` sized
  against it may need revisiting.

- **`stop_actor()` ran `on_shutdown()`/`on_stop()` concurrently with the
  actor's handlers.** The hooks ran on the caller's thread while the actor's
  worker could still be dispatching - the drain only waited until the last
  message was *dequeued*, and discard or async stops did not wait at all - so
  hooks raced handlers (an INVARIANT 1 violation the debug `dispatch_guard`
  could not see) and handlers could run after `on_stop()`. The stop now runs
  on the actor's own thread: `stop_actor()` flips the actor to `stopping` and
  the owning worker (or a fast actor's dedicated thread) cancels timers,
  drains or discards, runs the hooks, and marks it `stopped`. Consequences:
  - `stop_mode::discard` now actually drops queued messages (counted as dead
    letters); previously they were still processed.
  - `wait_for_stop = false` now returns without waiting; previously the whole
    stop ran synchronously and only the drain was skipped.
  - A fast actor's `on_stop()` runs once; it used to run twice, from the
    caller and from its own thread.
  - An actor can stop itself, or be stopped from its own worker thread,
    without deadlock: the call then completes asynchronously.
  - Fast-actor threads are joined outside every internal lock, fixing the
    `m_fast_actors_mutex` deadlock listed under 1.0.0's known issues.
  - An `inline_actor` rejects messages once stopping, and its stop is
    serialised with its synchronous dispatch; it also restores the previous
    sender context after a dispatch instead of clearing it, so a handler that
    tells an inline actor keeps its own identity as `sender` afterwards.
- **A stopping actor could unregister its replacement.** Its name was
  unregistered after watchers were notified, so a supervisor that restarted
  it under the same name from `termination_msg` lost the new registration.
  The name is now unregistered first, and only if it still refers to the
  stopping actor (new `actor_registry::unregister_actor(name, expected)`).

- **`call()` could pass garbage arguments to the handler.** The request stored
  its arguments as `std::tuple` of the *caller's* deduced types (references
  for lvalues) and the handler read them back as a tuple of *its* parameter
  types, with nothing checking the two agreed. Passing a variable
  (`call<int>(add_op{}, a, b)`), or a value needing conversion (`10` to a
  `double` parameter), silently produced wrong results; a mismatched
  `call<R>()` return type reinterpreted the result the same way. Arguments are
  now stored as decayed copies (so lvalues work, and cannot dangle when an
  call-with-timeout caller returns before the handler runs), and argument and
  return types are checked against the registered handler before it is
  invoked. A mismatch throws the new `cas::call_type_error` to the caller.
  Calls that previously worked are unaffected; calls that relied on implicit
  conversion now throw instead of returning wrong values — pass the exact
  parameter types.

## [1.0.0] - 2026-08-04

First stable release. The API is now considered settled; breaking changes
from here will come with a major version bump.

### Breaking

- **C++20 is now required** (previously C++17). The public headers use
  concepts, `operator<=>` and `std::jthread`, so consumers must compile as
  C++20 too. Minimum toolchains: **MSVC 2019 16.11+, GCC 13+, Clang 16+**.
  The requirement propagates through `target_compile_features(... PUBLIC
  cxx_std_20)`, so linking the CMake target is enough; adding the sources to
  a project by hand means setting the standard yourself.
- **`nlohmann_json` is no longer a dependency.** The ZeroMQ relay treats
  payloads as opaque bytes and never included it — JSON only appeared in
  documented examples of what a caller may put in a payload. Applications
  that want JSON should depend on it directly. This removes a package every
  consumer previously had to install for no benefit.
- **`handler<T>()` now requires `T` to derive from `message_base`.** A type
  that cannot reach a mailbox is rejected at the registration call rather
  than compiling into a handler that silently never fires. Existing code
  registering real message types is unaffected.

### Fixed

- **Timer callbacks could dereference a destroyed actor.** The timer thread
  copies a callback out of its map, releases the timer mutex, then invokes
  it; cancellation cannot reach a copy already extracted, and `stop_actor()`
  did not wait for one to finish. The callback captured the target as a raw
  pointer, so it could run against freed memory. It now captures a
  `weak_ptr` and locks it before delivery. Confirmed with AddressSanitizer:
  heap-use-after-free with the old capture, clean with the new one.
- **`timer_manager` destroyed its worker state before joining its thread.**
  The `std::jthread` was declared before the mutex, condition variable and
  containers its worker uses, and members are destroyed in reverse
  declaration order — so destruction tore that state down while the thread
  was still running. The thread is now the last-declared member and the
  destructor joins explicitly.
- **`timer_manager::is_running()` raced `start()`/`stop()`.** It inspected
  the `jthread` object, which is not safe to mutate and inspect
  concurrently. It reads an atomic again, as it did before the C++20 work.
- **A cancelled timer no longer delays the timer thread.** `cancel()` did
  not notify the condition variable, so a cancelled timer at the head of the
  queue kept its original fire time and the thread slept until it.
- **`fixed_string(nullptr)` is now usable in a constant expression.** The
  null path wrote only the terminator and left the rest of the buffer
  indeterminate, unlike every other constant-evaluated path.
- **Ask handlers no longer race the actor's own thread.** Ask requests are
  enqueued to the actor's own queue and drained on its owning thread ahead
  of the mailbox; the global ask queue and dedicated ask thread pool are
  gone. An actor with both an `ask_handler` and a regular handler previously
  had a silent data race on its members.
- **`worker_thread()` no longer holds the system mutex across user code.**
  It snapshots the per-thread actor list, releases the lock, then processes.
  Previously one system-wide mutex was taken on every polling iteration and
  held across message processing — bounding throughput, letting one slow
  handler stall all workers, and self-deadlocking any handler that called
  `system::create()`.

### Added

- **`tutorials/`** — fourteen runnable, self-checking programs in teaching
  order, from a first actor through to fast and inline actors. Each verifies
  its own behaviour and is registered as a `ctest` test, so an example that
  stops working fails the build rather than going quietly stale. Run
  `tutorials` to list them, `tutorials <n>` to run one.
- `fixed_string` gains `std::hash` and (where the standard library provides
  `<format>`) `std::formatter` specialisations, so it can be used as an
  unordered container key and formatted directly.
- `fixed_string` comparisons work in either operand order and across
  different capacities, and are usable at compile time.
- Timer teardown tests covering destruction while idle, with a pending
  one-shot, with an active periodic timer, and mid-callback.

### Changed

- `timer_manager` uses `std::jthread` and `std::stop_token` instead of a
  hand-rolled shutdown flag and manual join.
- Benchmarks in the README now name the hardware, cover both toolchains and
  show the range across runs, rather than quoting three unattributed
  figures.
- Documentation index corrected: three chapters were listed as "coming soon"
  despite being written, and three "Next Steps" links pointed at files that
  do not exist.

### Known issues

- **`system::stop_actor()` holds `m_fast_actors_mutex` across
  `std::thread::join()`** (`source/system.cpp`, step 8). If user code
  running in a fast actor's `on_stop()` — or in a handler it is still
  draining — calls back into the system in a way that needs that mutex
  (creating another fast actor, or `system::actor_count()`), the two
  deadlock: `stop_actor()` waits for the thread while the thread waits for
  the mutex. This predates 1.0.0 and is not a regression. Avoid calling back
  into the actor system from a fast actor's `on_stop()` until it is fixed.
  Ordinary pooled actors are not affected.
- The timer-lifetime regression tests
  (`tests/06_timers/test_timer_actor_lifetime.cpp`) exercise the right
  window but catch a reintroduction only opportunistically — they race and
  usually lose. Closing that properly needs a test seam inside
  `timer_manager` between callback extraction and invocation.
- `tests/04_advanced/test_stateful_actor.cpp` is excluded from the build for
  ABI sensitivity, so `stateful_actor` is documented but not covered by the
  suite.
