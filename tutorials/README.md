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

| # | Tutorial | Teaches |
|---|----------|---------|
| 1 | `hello_actor` | Messages, actors, handlers, system lifecycle |
| 2 | `message_passing` | Multiple message types, actor state, unhandled messages |
| 3 | `actor_to_actor` | Actors messaging each other, replying via `msg.sender` |

Each assumes the ones before it and does not repeat their explanations.

## Conventions

**Handlers are member functions** by default. `on_start()` then reads as a
list of the messages an actor accepts, with the logic in named methods.
Lambdas appear where a handler is a genuine one-liner — tutorial 2 shows both
side by side.

**Tutorials observe results through a file-scope atomic.** `create<T>()`
returns an `actor_ref`, not the actor, so there is no way to reach in and read
its state; the handler records what happened somewhere `main()` can see. Real
programs usually reply with a message instead (tutorial 3).

**Waiting is by polling, not by sleeping.** `wait_until()` polls for the
condition so the tutorials are not flaky on a loaded machine or slow on a fast
one.

## Adding a tutorial

1. Add `NN_name.cpp` with a `run_name()` function in `namespace tut`.
2. Declare it in `tutorials.h`.
3. Add it to `g_tutorials[]` in `main.cpp`.
4. Add the file to `TUTORIAL_SOURCES` and the name to `TUTORIAL_NAMES` in
   `CMakeLists.txt`.
