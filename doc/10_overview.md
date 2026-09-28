# Cygnus Actor Framework - Overview

## What is Cygnus?

Cygnus is a modern C++20 actor framework designed for high-performance concurrent applications. It provides a message-passing model that eliminates shared mutable state and makes concurrent programming safer and more intuitive.

## Key Features

### Core Capabilities
- **Actor-based concurrency** - No locks, no data races, just messages
- **Type-safe message passing** - Compile-time type checking for all messages
- **Actor registry** - Name-based actor discovery and lookup
- **Lifecycle hooks** - Controlled initialization and cleanup
- **Graceful shutdown** - Queued messages are drained, up to `shutdown_config::drain_timeout` (default 5 s); any left are reported by `system::get_shutdown_log()`
- **Dynamic actor management** - Create and stop actors at runtime

### Advanced Features
- **Call (RPC)** - Synchronous request-response calls (named `ask()` in 1.0; the old names still compile, with a deprecation warning)
- **Timers** - One-shot and periodic message scheduling
- **Fast actors** - Dedicated threads for low-latency processing
- **Inline actors** - Zero-latency synchronous execution
- **Stateful actors** - Selective receive (`stateful_actor`, or `stateful_fast_actor` on a dedicated thread)

### Performance
- **Lock-free mailboxes** - An intrusive lock-free MPSC mailbox per actor (adapted from CAF); pooled actors are scheduled from per-worker lock-free ready queues
- **Thread affinity** - Each actor bound to one thread (no context switching)
- **Pooled messages** - Each send copies the message once into a size-class pool block (see [Message Pool](140_message_pool.md))
- **Minimal overhead** - Lightweight actor references

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    Actor System                         │
│  ┌───────────────────────────────────────────────────┐  │
│  │  Thread Pool (configurable size)                  │  │
│  │  ┌──────────┐ ┌──────────┐ ┌──────────┐          │  │
│  │  │ Thread 1 │ │ Thread 2 │ │ Thread N │          │  │
│  │  └────┬─────┘ └────┬─────┘ └────┬─────┘          │  │
│  └───────┼────────────┼────────────┼────────────────┘  │
│          │            │            │                    │
│    ┌─────▼──┐   ┌────▼───┐  ┌────▼───┐               │
│    │Actor A │   │Actor B │  │Actor C │               │
│    │Mailbox │   │Mailbox │  │Mailbox │               │
│    └────────┘   └────────┘  └────────┘               │
│                                                         │
│  ┌───────────────────────────────────────────────────┐  │
│  │  Actor Registry (name → actor_ref lookup)         │  │
│  └───────────────────────────────────────────────────┘  │
│                                                         │
│  ┌───────────────────────────────────────────────────┐  │
│  │  Timer Manager (scheduled message delivery)       │  │
│  └───────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────┘
```

## Core Concepts

### Actors
Self-contained units of computation with:
- Private state (no shared memory)
- Message mailbox (lock-free; handled in send order per sender, calls first)
- Message handlers (process incoming messages)
- Lifecycle hooks (initialization and cleanup)

### Messages
Immutable data structures passed between actors:
- Must inherit from `cas::message_base`
- Automatically include sender information
- Type-safe dispatch to handlers

### Actor References
Lightweight handles for sending messages:
- Thread-safe
- Copyable and assignable
- Type-erased (works with any actor type)

## Quick Example

```cpp
#include "cas/cas.h"

// Define a message
struct greeting : public cas::message_base {
    std::string text;
};

// Define an actor
class greeter : public cas::actor {
protected:
    void on_start() override {
        set_name("greeter");
        handler<greeting>(&greeter::on_greeting);
    }

    void on_greeting(const greeting& msg) {
        std::cout << "Received: " << msg.text << std::endl;
    }
};

int main() {
    // Create actor
    auto actor = cas::system::create<greeter>();

    // Start system
    cas::system::start();

    // Send message
    greeting msg;
    msg.text = "Hello, Cygnus!";
    actor.tell(msg);

    // Graceful shutdown
    cas::system::shutdown();
    cas::system::wait_for_shutdown();

    return 0;
}
```

## Use Cases

### Ideal For
- **Concurrent servers** - Handle many clients simultaneously
- **Real-time systems** - Low-latency message processing with fast actors
- **State machines** - Use stateful actors for protocol implementation
- **Event processing** - Asynchronous event-driven architectures
- **Simulation** - Each entity is an actor
- **Trading systems** - High-frequency message processing

### Not Ideal For
- **CPU-bound parallel computing** - Use thread pools or parallel algorithms instead
- **Single-threaded applications** - Overhead not justified
- **Shared memory algorithms** - Actor model prohibits shared state

## Thread Model

### Pooled Actors (Default)
- Actors assigned to thread pool in round-robin fashion
- Each actor pinned to one thread (thread affinity)
- Multiple actors per thread
- A worker runs only actors on its ready queue - an actor is queued when a message finds it idle; workers never scan all their actors
- An actor handles up to `system_config::max_messages_per_turn` (default 64) messages per turn, then other ready actors on its worker get a turn
- An idle worker spins for `system_config::worker_idle_spin` (default 20 µs) before parking, so work arriving meanwhile needs no OS wake-up
- Good for most use cases

### Fast Actors
- One dedicated thread per actor
- Ultra-low latency (<1μs)
- Configurable polling strategies
- Use sparingly (higher resource usage)

### Inline Actors
- Execute in caller's thread
- Zero latency (no queuing)
- Thread-safe and non-thread-safe variants
- A handler may tell its own inline actor: the message is handled right after the current handler, before the outer `tell()` returns
- Ideal for synchronous operations

## Message Delivery Guarantees

- **Exactly once to a live actor** - Nothing lost, nothing duplicated
- **Send order per sender** - Messages from A to B are handled in the order A sent them; an `call()` is handled next, ahead of queued messages
- **No global ordering** - Messages from different senders may interleave
- **Dead letters** - Messages to stopping or stopped actors, or sent through an invalid `actor_ref`, are dropped and counted (`system::get_dead_letter_stats()`, optional handler via `system::set_dead_letter_handler()`)

## Naming Conventions

Cygnus follows strict naming conventions:

- **All identifiers** - `snake_case` (no camelCase)
- **Member variables** - `m_` prefix without trailing underscore
- **Struct members** - No prefix (plain names)
- **Accessor methods** - Property-style naming (no `get_`/`set_`)

Example:
```cpp
class my_actor : public cas::actor {
private:
    int m_threshold = 0;  // Member variable
    int m_counter = 0;    // Member variable

protected:
    void on_start() override {}

public:
    // Property-style accessor
    int threshold() const { return m_threshold; }
    void threshold(int t) { m_threshold = t; }
};

struct my_message : public cas::message_base {
    int value;       // Struct member (no prefix)
    std::string data; // Struct member (no prefix)
};
```

## License

MIT - see [LICENSE](../LICENSE). Third-party code (CAF, moodycamel) - see [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).

## Next Steps

- [Getting Started Guide](20_getting_started.md) - Build your first actor application
- [Tutorials](../tutorials/) - Fourteen runnable programs, in order
- [Message Passing](40_message_passing.md) - Master message handling
