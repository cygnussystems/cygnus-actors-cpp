# Cygnus Actor Framework - Overview

## What is Cygnus?

Cygnus is a modern C++17 actor framework designed for high-performance concurrent applications. It provides a message-passing model that eliminates shared mutable state and makes concurrent programming safer and more intuitive.

## Key Features

### Core Capabilities
- **Actor-based concurrency** - No locks, no data races, just messages
- **Type-safe message passing** - Compile-time type checking for all messages
- **Actor registry** - Name-based actor discovery and lookup
- **Lifecycle hooks** - Controlled initialization and cleanup
- **Graceful shutdown** - All messages processed before termination
- **Dynamic actor management** - Create and stop actors at runtime

### Advanced Features
- **Ask pattern (RPC)** - Synchronous request-response calls
- **Timers** - One-shot and periodic message scheduling
- **Fast actors** - Dedicated threads for low-latency processing
- **Inline actors** - Zero-latency synchronous execution
- **Stateful actors** - State-based message filtering

### Performance
- **Lock-free queues** - High-throughput message delivery using moodycamel::ConcurrentQueue
- **Thread affinity** - Each actor bound to one thread (no context switching)
- **Zero-copy messages** - Messages moved, not copied
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
- Message mailbox (lock-free queue)
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
- Ideal for synchronous operations

## Message Delivery Guarantees

- **At-most-once** - Messages may be lost if actor stops
- **FIFO order** - Messages from A to B delivered in send order
- **No global ordering** - Message interleaving between different senders
- **Best-effort delivery** - Messages to stopped actors are dropped

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
    std::string m_name;  // Member variable
    int m_counter;       // Member variable

public:
    // Property-style accessor
    std::string name() const { return m_name; }
    void name(const std::string& n) { m_name = n; }
};

struct my_message : public cas::message_base {
    int value;       // Struct member (no prefix)
    std::string data; // Struct member (no prefix)
};
```

## License

[Add your license information here]

## Next Steps

- [Getting Started Guide](20_getting_started.md) - Build your first actor application
- [Creating Actors](30_creating_actors.md) - Learn actor creation patterns
- [Message Passing](40_message_passing.md) - Master message handling
