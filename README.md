# Cygnus Actor Framework

A modern C++17 actor framework for high-performance concurrent applications. Cygnus provides a message-passing model that eliminates shared mutable state and makes concurrent programming safer and more intuitive.

## Key Features

### Core Capabilities
- **Actor-based concurrency** - No locks, no data races, just messages
- **Type-safe message passing** - Compile-time type checking for all messages
- **Lock-free queues** - High-throughput using moodycamel::ConcurrentQueue
- **Thread affinity** - Each actor bound to one thread (no context switching)
- **Actor registry** - Name-based actor discovery and lookup
- **Graceful shutdown** - All messages processed before termination

### Advanced Features
- **Ask pattern (RPC)** - Synchronous request-response with timeout support
- **Timers** - One-shot and periodic message scheduling
- **Dynamic actor management** - Create and stop actors at runtime
- **Watch pattern** - Get notified when actors terminate
- **Fast actors** - Dedicated threads for ultra-low latency (<1μs)
- **Inline actors** - Zero-latency synchronous execution
- **Stateful actors** - State-based message filtering

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                      Actor System                           │
│  ┌───────────────────────────────────────────────────────┐  │
│  │  Thread Pool (configurable size)                      │  │
│  │  ┌──────────┐ ┌──────────┐ ┌──────────┐              │  │
│  │  │ Thread 1 │ │ Thread 2 │ │ Thread N │              │  │
│  │  └────┬─────┘ └────┬─────┘ └────┬─────┘              │  │
│  └───────┼────────────┼────────────┼────────────────────┘  │
│          │            │            │                        │
│    ┌─────▼──┐   ┌────▼───┐  ┌────▼───┐                   │
│    │Actor A │   │Actor B │  │Actor C │                   │
│    │Mailbox │   │Mailbox │  │Mailbox │                   │
│    └────────┘   └────────┘  └────────┘                   │
│                                                             │
│  ┌───────────────────────────────────────────────────────┐  │
│  │  Actor Registry (name → actor_ref lookup)             │  │
│  └───────────────────────────────────────────────────────┘  │
│                                                             │
│  ┌───────────────────────────────────────────────────────┐  │
│  │  Timer Manager (scheduled message delivery)           │  │
│  └───────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

## Quick Start

### 1. Define Your Messages

Messages are simple structs that inherit from `cas::message_base`:

```cpp
#include "cas/cas.h"

struct greeting : public cas::message_base {
    std::string text;
    int count;
};

struct response : public cas::message_base {
    bool success;
};
```

### 2. Create Your Actor

Actors inherit from `cas::actor` and register message handlers in `on_start()`:

```cpp
class greeter : public cas::actor {
protected:
    void on_start() override {
        set_name("greeter");

        // Register handler using method pointer
        handler<greeting>(&greeter::on_greeting);
    }

    void on_shutdown() override {
        // Called before stopping - can still send messages
    }

    void on_stop() override {
        // Final cleanup - no message sending allowed
    }

private:
    void on_greeting(const greeting& msg) {
        std::cout << "Received: " << msg.text << " (count: " << msg.count << ")\n";

        // Reply to sender
        if (msg.sender.is_valid()) {
            response reply;
            reply.success = true;
            msg.sender.tell(reply);
        }
    }
};
```

### 3. Run the System

```cpp
int main() {
    // Create actors
    auto greeter_ref = cas::system::create<greeter>();

    // Start the system
    cas::system::start();

    // Send messages
    greeting msg;
    msg.text = "Hello, Cygnus!";
    msg.count = 1;
    greeter_ref.tell(msg);

    // Graceful shutdown
    cas::system::shutdown();
    cas::system::wait_for_shutdown();

    return 0;
}
```

## Actor-to-Actor Communication

Actors can communicate by looking up other actors in the registry:

```cpp
class ping_actor : public cas::actor {
    cas::actor_ref pong_ref;

protected:
    void on_start() override {
        set_name("ping");
        handler<pong_message>(&ping_actor::on_pong);

        // Look up another actor by name
        pong_ref = cas::actor_registry::get("pong");

        // Send initial message
        ping_message msg;
        msg.id = 1;
        pong_ref.tell(msg);
    }

    void on_pong(const pong_message& msg) {
        std::cout << "Got pong: " << msg.id << "\n";
    }
};
```

## Ask Pattern (RPC-Style Calls)

Make synchronous calls with timeout support:

```cpp
// Define operation tag and handler
struct calculate_op {};

class calculator : public cas::actor {
protected:
    void on_start() override {
        set_name("calculator");
        invoke_handler<double, calculate_op>(&calculator::do_calculate);
    }

    double do_calculate(int value) {
        return value * 2.5;
    }
};

// Call synchronously from another actor or main
auto calc = cas::system::create<calculator>();
cas::system::start();

// Blocking call with 5 second timeout
auto result = calc.ask<double, calculate_op>(42, std::chrono::seconds(5));
if (result) {
    std::cout << "Result: " << *result << "\n";  // 105.0
}
```

## Timers

Schedule delayed or periodic messages:

```cpp
class timer_actor : public cas::actor {
protected:
    void on_start() override {
        handler<tick_message>(&timer_actor::on_tick);

        // One-shot timer (fires once after 1 second)
        schedule_once<tick_message>(std::chrono::seconds(1));

        // Periodic timer (fires every 500ms)
        auto timer_id = schedule_periodic<heartbeat>(
            std::chrono::milliseconds(500)
        );

        // Cancel a timer
        cancel_timer(timer_id);
    }

    void on_tick(const tick_message& msg) {
        std::cout << "Tick!\n";
    }
};
```

## Dynamic Actor Management

Create and stop actors at runtime:

```cpp
// Create actor dynamically
auto worker = cas::system::create<worker_actor>();

// Stop an actor
cas::system::stop_actor(worker);

// Stop by name
cas::system::stop_actor("worker");

// Check if still running
if (worker.is_running()) {
    // Still active
}

// Watch for termination
cas::system::watch(supervisor_ref, worker);  // supervisor gets notified when worker stops
```

## Actor Types

### Pooled Actors (Default)
Standard actors that share a thread pool. Good for most use cases.

```cpp
auto actor = cas::system::create<my_actor>();
```

### Fast Actors
Dedicated thread per actor for ultra-low latency (<1μs):

```cpp
class fast_processor : public cas::fast_actor {
    // Same API as regular actor
};

auto fast = cas::system::create<fast_processor>();
```

### Inline Actors
Execute in the caller's thread (zero latency):

```cpp
class sync_handler : public cas::inline_actor {
    // Handlers execute synchronously when tell() is called
};
```

## Thread Model

| Actor Type | Threading | Latency | Use Case |
|------------|-----------|---------|----------|
| Pooled (default) | Shared thread pool | Low | General purpose |
| Fast | Dedicated thread | Ultra-low (<1μs) | Latency-critical |
| Inline | Caller's thread | Zero | Synchronous operations |

## Message Delivery Guarantees

- **At-most-once** - Messages may be lost if actor stops
- **FIFO order** - Messages from A to B delivered in send order
- **No global ordering** - Message interleaving between different senders
- **Best-effort delivery** - Messages to stopped actors are dropped

## Building

```bash
# Configure
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build

# Run tests
./build/unit_tests
```

### Requirements

- C++17 compiler (MSVC 2017+, GCC 7+, Clang 5+)
- CMake 3.14+

### Platform Support

- **Windows**: MSVC 2017+, Visual Studio 2022 recommended
- **Linux**: GCC 7+, Clang 5+
- **macOS**: Clang 5+ (Xcode 9.3+)

## Documentation

Full documentation is available in the [doc/](doc/) folder:

| Document | Description |
|----------|-------------|
| [Overview](doc/10_overview.md) | Architecture and core concepts |
| [Getting Started](doc/20_getting_started.md) | Quick start guide with examples |
| [Message Passing](doc/40_message_passing.md) | Message handlers and sending patterns |
| [Lifecycle Hooks](doc/50_lifecycle_hooks.md) | on_start, on_shutdown, on_stop |
| [Actor Registry](doc/60_actor_registry.md) | Name-based actor lookup |
| [Dynamic Removal](doc/70_dynamic_removal.md) | Stop actors at runtime |
| [Ask Pattern](doc/80_ask_pattern.md) | RPC-style synchronous calls |
| [Timers](doc/90_timers.md) | One-shot and periodic scheduling |
| [Advanced Actors](doc/100_advanced_actors.md) | Fast, inline, and stateful actors |
| [Best Practices](doc/120_best_practices.md) | Design patterns and tips |

## Use Cases

### Ideal For
- **Concurrent servers** - Handle many clients simultaneously
- **Real-time systems** - Low-latency processing with fast actors
- **Event processing** - Asynchronous event-driven architectures
- **State machines** - Protocol implementation with stateful actors
- **Trading systems** - High-frequency message processing
- **Simulations** - Each entity as an independent actor

### Not Ideal For
- CPU-bound parallel computing (use thread pools instead)
- Single-threaded applications (overhead not justified)
- Algorithms requiring shared memory

## Naming Conventions

Cygnus follows strict `snake_case` naming:

```cpp
class my_actor : public cas::actor {
private:
    std::string m_name;      // Member variables: m_ prefix
    int m_counter;

public:
    std::string name() const { return m_name; }  // Property-style accessors
    void name(const std::string& n) { m_name = n; }
};

struct my_message : public cas::message_base {
    int value;               // Struct members: no prefix
    std::string data;
};
```

## License

[MIT License](LICENSE)

## Contributing

Contributions are welcome! Please see the documentation for coding conventions and submit pull requests to the main branch.
