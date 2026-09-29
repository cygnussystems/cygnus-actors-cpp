# Tutorials

Runnable, self-checking examples of the framework API. Work through them in
order and you should be able to use the library.

Each tutorial is a complete program with a heavily commented source file. Read
the source alongside the output — the console sections correspond to the
sections in the file.

## Running

```bash
# List the available tutorials
./tutorials

# Run one and exit
./tutorials 1
```

One tutorial runs per process. That is deliberate: the actor system is a
process-wide singleton, so a fresh process per tutorial means each one starts
from a pristine system and no tutorial needs teardown code that would distract
from what it is teaching.

## Why they check themselves

Every tutorial verifies its own behaviour and exits non-zero if a check fails,
and each is registered as a separate `ctest` test:

```bash
ctest -R tutorial
```

A tutorial that prints the right thing and then deadlocks looks identical to a
working one in a CI log. Checking the outcome means these fail when the
framework's behaviour changes, not merely when the API stops compiling — so
the examples cannot quietly go stale.

## Order

**Core model — read these in order.**

| # | Tutorial | Teaches |
|---|----------|---------|
| 1 | `hello_actor` | Messages, actors, handlers, system lifecycle |
| 2 | `message_passing` | Multiple message types, actor state, unhandled messages |
| 3 | `actor_to_actor` | Actors messaging each other, replying via `msg.sender` |
| 4 | `lifecycle_hooks` | `on_start` / `on_shutdown` / `on_stop`, message draining |

**Everyday patterns.**

| # | Tutorial | Teaches |
|---|----------|---------|
| 5 | `actor_registry` | Finding actors by name instead of holding a reference |
| 6 | `call` | Request/response with a return value, timeouts, deadlock rules |
| 7 | `timers` | One-shot and periodic timers, cancellation |
| 8 | `dynamic_removal` | Stopping one actor while the system runs; drain vs discard |
| 9 | `watch_pattern` | Being notified when a watched actor terminates |
| 10 | `dead_letters` | Seeing messages that could not be delivered |

**Performance and specialised actors.**

| # | Tutorial | Teaches |
|---|----------|---------|
| 11 | `fixed_string` | Inline strings, for messages that do not allocate |
| 12 | `message_pool` | How messages are allocated; keeping them on the fast path |
| 13 | `fast_actor` | A dedicated thread, for latency-critical work |
| 14 | `inline_actor` | Synchronous dispatch in the sender's thread |

**Putting it together.**

| # | Tutorial | Teaches |
|---|----------|---------|
| 15 | `trading_manager` | A manager that creates actors at runtime (a trading system plus one stream per subscriber), watches them, and retires everything that belonged to a trading system when it completes or fails |
| 16 | `stateful_actor` | Selective receive: an exchange session that holds orders, in order, until it is logged in; `stateful_fast_actor` |
| 17 | `zeromq_relay` | Talking to other processes: a server and a client relay, routing by message type, request/reply by correlation id, pushes, errors, status. Built only with ZeroMQ (`ENABLE_ZEROMQ`, vcpkg `cppzmq`) |

Each assumes the ones before it and does not repeat their explanations.

## Not covered

Everything in the framework has a tutorial. Tutorial 17 (the ZeroMQ relay) is
built only when the framework is built with ZeroMQ; see
[doc/130_zeromq_relay.md](../doc/130_zeromq_relay.md).

## Conventions

**An actor is self-contained.** Each actor class holds its messages, its
state, its handlers and what it exposes. A message only one actor receives is
a nested type of that actor (`greeter::greet`); messages exchanged between
actors - a request and its reply, a shared event - sit in a short protocol
block at the top of the file.

**No global state.** `create<T>()` returns an `actor_ref`, not the actor, so
`main()` cannot reach in and read actor state. What a tutorial needs to check
is published by the actor that owns it, as `static inline std::atomic`
members marked "observed by the tutorial". Real programs reply with a message
instead (tutorial 3).

**Plain syntax: no lambdas.** Handlers are private member functions, so
`on_start()` reads as the list of messages the actor accepts; callbacks are
static member functions; waits name what they wait for -
`wait_until(greeter::greetings, 1)`, `wait_until(&session::order_count, 3)`.

**Each step is a `heading()`** - underlined in the output and by a
`//----` comment in the source, so code and output line up.

**Waiting is by polling, not by sleeping.** `wait_until()` polls for the
condition, so the tutorials are neither flaky on a loaded machine nor slow on
a fast one.

## Adding a tutorial

1. Add `NN_name.cpp` with a `run_name()` function in `namespace tut`.
2. Declare it in `tutorials.h`.
3. Add it to `g_tutorials[]` in `main.cpp`.
4. Add the file to `TUTORIAL_SOURCES` and the name to `TUTORIAL_NAMES` in
   `CMakeLists.txt`.
