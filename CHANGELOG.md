# Changelog

All notable changes to the Cygnus Actor Framework are documented here.

This project follows [Semantic Versioning](https://semver.org/).

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
