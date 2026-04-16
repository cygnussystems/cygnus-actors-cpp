# Cygnus Actor Framework

A modern C++17 actor framework built for **speed without sacrificing simplicity**.

Most actor frameworks force you to choose: either raw performance with complex APIs, or clean syntax with overhead. Cygnus was designed to deliver both - **5+ million messages/sec** with an API that feels natural to C++ developers and can be learned in an afternoon.

### Why Cygnus?

- **Blazing Fast**: Lock-free message queues, zero-allocation message pooling, sub-200ns latency
- **Intuitive API**: Define actors in ~10 lines. No boilerplate, no macros, no inheritance hierarchies
- **Easy Learning Curve**: If you know C++ lambdas and inheritance, you already know 80% of Cygnus
- **Production Ready**: Timer scheduling, RPC-style ask pattern, actor supervision, dead letter handling

```cpp
// This is a complete actor. Really.
class greeter : public cas::actor {
    void on_start() override {
        handler<greet>([](const greet& msg) {
            std::cout << "Hello, " << msg.name << "!\n";
        });
    }
};
```

## Table of Contents

- [Installation](#installation)
- [Quick Start](#quick-start)
- [System Lifecycle](#system-lifecycle)
- [Messages](#messages)
- [Actors](#actors)
- [Actor Communication](#actor-communication)
- [Memory Pool](#memory-pool)
- [Timers](#timers)
- [Ask Pattern (RPC)](#ask-pattern-rpc)
- [Actor Types](#actor-types)
- [Common Patterns](#common-patterns)
- [Troubleshooting](#troubleshooting)
- [Benchmarks](#benchmarks)
- [API Reference](#api-reference)

## Installation

### Requirements
- C++17 compiler (MSVC 2017+, GCC 7+, Clang 5+)
- CMake 3.14+

### Building

```bash
# Clone the repository
git clone https://github.com/cygnussystems/cygnus-actors-cpp.git
cd cygnus-actors-cpp

# Configure and build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Run tests
./build/unit_tests

# Run benchmarks
./build/benchmark
```

### Integration

Add to your CMakeLists.txt:
```cmake
add_subdirectory(cygnus-actors-cpp)
target_link_libraries(your_target CygnusActorFramework)
```

Or simply include the headers and source files in your project.

## Quick Start

```cpp
#include "cas/cas.h"
#include <iostream>

// 1. Define a message
struct greet : cas::message_base {
    std::string name;
};

// 2. Define an actor
class greeter : public cas::actor {
protected:
    void on_start() override {
        set_name("greeter");
        handler<greet>(&greeter::on_greet);
    }

    void on_greet(const greet& msg) {
        std::cout << "Hello, " << msg.name << "!\n";
    }
};

// 3. Run
int main() {
    // Create actors BEFORE starting the system
    auto greeter_ref = cas::system::create<greeter>();

    // Start the system (begins processing messages)
    cas::system::start();

    // Send a message
    greet msg;
    msg.name = "World";
    greeter_ref.tell(msg);

    // Give time for processing (in real apps, use proper synchronization)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Shutdown gracefully
    cas::system::shutdown();
    cas::system::wait_for_shutdown();

    return 0;
}
```

## System Lifecycle

Understanding the correct sequence of operations is critical:

```
┌─────────────────────────────────────────────────────────────┐
│                    SYSTEM LIFECYCLE                          │
├─────────────────────────────────────────────────────────────┤
│  1. CREATE ACTORS     cas::system::create<MyActor>()        │
│         ↓             (actors exist but don't process yet)  │
│  2. START SYSTEM      cas::system::start()                  │
│         ↓             (worker threads begin, on_start runs) │
│  3. SEND MESSAGES     actor_ref.tell(msg)                   │
│         ↓             (normal operation)                     │
│  4. SHUTDOWN          cas::system::shutdown()               │
│         ↓             (stops accepting, drains queues)      │
│  5. WAIT              cas::system::wait_for_shutdown()      │
│                       (blocks until all actors stopped)      │
└─────────────────────────────────────────────────────────────┘
```

### Important Rules

1. **Create actors before `start()`** - Actors created after start() work but may miss early messages
2. **Call `shutdown()` before `wait_for_shutdown()`** - wait blocks until shutdown is initiated
3. **Don't send messages after `shutdown()`** - Messages to stopped actors are dropped
4. **Actors can create other actors** - Use `cas::system::create<T>()` from within an actor

### Example: Proper Lifecycle

```cpp
int main() {
    // Phase 1: Create all initial actors
    auto logger = cas::system::create<logger_actor>();
    auto processor = cas::system::create<processor_actor>();
    auto monitor = cas::system::create<monitor_actor>();

    // Phase 2: Start the system
    cas::system::start();

    // Phase 3: Normal operation
    // ... your application logic ...
    // Actors process messages concurrently

    // Phase 4: Initiate shutdown
    cas::system::shutdown();
    // At this point:
    // - No new external messages accepted
    // - Each actor receives system_shutdown message
    // - Actors finish processing their queues
    // - on_shutdown() called, then on_stop()

    // Phase 5: Wait for completion
    cas::system::wait_for_shutdown();
    // All actors have stopped, safe to exit

    return 0;
}
```

## Messages

Messages are structs that inherit from `cas::message_base`:

```cpp
struct order : cas::message_base {
    std::string symbol;
    int quantity;
    double price;
};
```

### Message Fields (Automatic)

Every message automatically has:
```cpp
struct message_base {
    actor_ref sender;        // Who sent this (set by framework)
    uint64_t message_id;     // Unique ID (set by framework)
    uint64_t correlation_id; // For request/response tracking
};
```

### Replying to Messages

```cpp
void on_order(const order& msg) {
    // Process the order...

    // Reply to sender
    if (msg.sender.is_valid()) {
        order_confirmation reply;
        reply.order_id = 12345;
        msg.sender.tell(reply);
    }
}
```

### System Messages

The framework sends these automatically:
- `cas::system_shutdown` - Sent to all actors during shutdown
- `cas::termination_msg` - Sent to watchers when a watched actor stops
- `cas::queue_threshold_warning` - Sent when an actor's queue exceeds threshold

## Actors

### Basic Actor Structure

```cpp
class my_actor : public cas::actor {
protected:
    // Called when actor starts (register handlers here)
    void on_start() override {
        set_name("my_actor");  // Register in actor registry

        // Register message handlers
        handler<message_a>(&my_actor::handle_a);
        handler<message_b>(&my_actor::handle_b);
    }

    // Called during shutdown (can still send messages)
    void on_shutdown() override {
        // Save state, notify others, etc.
    }

    // Called after shutdown (no message sending allowed)
    void on_stop() override {
        // Final cleanup, release resources
    }

private:
    void handle_a(const message_a& msg) {
        // Process message_a
    }

    void handle_b(const message_b& msg) {
        // Process message_b
    }

    // Actor state (thread-safe - only this actor accesses it)
    int m_counter = 0;
    std::string m_data;
};
```

### Handler Registration

```cpp
void on_start() override {
    // Method 1: Member function pointer (recommended)
    handler<my_message>(&my_actor::on_my_message);

    // Method 2: Lambda
    handler<other_message>([this](const other_message& msg) {
        // Handle inline
    });
}
```

### Actor Naming

Actors can be named for discovery via the registry:

```cpp
void on_start() override {
    set_name("order_processor");  // Now findable by name
}

// Elsewhere:
auto processor = cas::actor_registry::get("order_processor");
if (processor.is_valid()) {
    processor.tell(some_message{});
}
```

## Actor Communication

### Direct Reference (Preferred)

```cpp
// Keep a reference from creation
auto worker = cas::system::create<worker_actor>();
worker.tell(task{});
```

### Registry Lookup

```cpp
// Look up by name
auto logger = cas::actor_registry::get("logger");
if (logger.is_valid()) {
    logger.tell(log_message{"Something happened"});
}
```

### Actor-to-Actor

```cpp
class supervisor : public cas::actor {
    cas::actor_ref m_worker;

protected:
    void on_start() override {
        set_name("supervisor");

        // Create a child actor
        m_worker = cas::system::create<worker_actor>();

        // Or look up existing
        m_worker = cas::actor_registry::get("worker");

        handler<result>(&supervisor::on_result);
    }

    void on_result(const result& msg) {
        // msg.sender is automatically set to whoever sent this
        std::cout << "Got result from: " << msg.sender.name() << "\n";
    }
};
```

### Checking Actor State

```cpp
actor_ref ref = cas::actor_registry::get("some_actor");

if (ref.is_valid()) {
    // Reference exists and points to an actor object
    // (actor may be stopped but object still exists)
}

if (ref.is_running()) {
    // Actor is currently active and processing messages
}
```

### Sending to Stopped Actors

```cpp
auto worker = cas::system::create<worker_actor>();
cas::system::start();

// Later: stop the actor
cas::system::stop_actor(worker);

// What happens now?
worker.tell(some_message{});  // Message is SILENTLY DROPPED

// The actor_ref is still valid (points to actor object)
worker.is_valid();    // true - object exists
worker.is_running();  // false - actor is stopped

// Safe pattern: check before sending
if (worker.is_running()) {
    worker.tell(msg);
} else {
    // Handle actor being down
}
```

**Important**: Messages to stopped actors are silently dropped - no exception, no error. Check `is_running()` if you need guaranteed delivery.

## Memory Pool

Cygnus uses a lock-free memory pool for message allocation, eliminating heap allocation overhead.

### How It Works

```
┌─────────────────────────────────────────────────────────────┐
│                    MESSAGE POOL                              │
├─────────────────────────────────────────────────────────────┤
│  Size Classes: 64, 128, 256, 512, 1024 bytes                │
│                                                              │
│  Allocation:                                                 │
│    1. Find size class that fits message                     │
│    2. Pop block from lock-free free-list (fast path)        │
│    3. If empty, allocate from heap (slow path)              │
│                                                              │
│  Deallocation:                                               │
│    1. Push block back to free-list (if under limit)         │
│    2. Or free to OS (if pool is full)                       │
│                                                              │
│  Messages > 1024 bytes: Always use heap                     │
└─────────────────────────────────────────────────────────────┘
```

### Zero-Allocation Messages

The pool is automatic - just use messages normally:

```cpp
my_message msg;
msg.data = 42;
actor_ref.tell(msg);  // Uses pool automatically
```

### Fixed-Capacity Strings

For truly zero-allocation messages, use `cas::fixed_string<N>`:

```cpp
// BAD for HFT: std::string allocates on heap
struct slow_message : cas::message_base {
    std::string symbol;      // Heap allocation!
    std::string client_id;   // Heap allocation!
};

// GOOD for HFT: fixed_string is inline
struct fast_message : cas::message_base {
    cas::fixed_string<8> symbol;      // Inline, no allocation
    cas::fixed_string<16> client_id;  // Inline, no allocation
    int64_t quantity;
    int64_t price;
};
```

`fixed_string<N>` API:
```cpp
cas::fixed_string<32> str;

// Assignment
str = "hello";
str = std::string("world");
str = std::string_view("test");

// Access
str.size();          // Current length
str.capacity();      // Maximum length (N)
str.empty();         // Is empty?
str.c_str();         // Null-terminated C string
str[0];              // Character access
str.at(0);           // Bounds-checked access

// Modification
str.clear();
str.push_back('!');
str.append(" world");
str += "!";

// Comparison (works with string_view, string, char*)
if (str == "hello") { }
if (str < other_str) { }

// Conversion
std::string s = str.str();           // Copy to std::string
std::string_view sv = str.view();    // View (no copy)
std::string_view sv2 = str;          // Implicit conversion
```

**Note**: `fixed_string` silently truncates if content exceeds capacity (no exceptions for performance).

### Pool Configuration

```cpp
// Default: 10,000 blocks per size class (~25MB max pool memory)

// Increase for high-throughput systems
cas::message_pool::set_max_pool_size(100000);

// Decrease for memory-constrained systems
cas::message_pool::set_max_pool_size(1000);

// Unlimited (pool grows forever - use with caution)
cas::message_pool::set_max_pool_size(0);

// Pre-warm pool at startup (avoids first-message latency)
cas::message_pool::prewarm(1000);  // 1000 blocks per size class
```

### Monitoring Pool Performance

```cpp
auto stats = cas::message_pool::get_stats();

std::cout << "Pool hits: " << stats.pool_hits << "\n";        // Fast allocations
std::cout << "Pool misses: " << stats.pool_misses << "\n";    // Needed new block
std::cout << "Heap fallbacks: " << stats.heap_fallbacks << "\n";  // >1KB messages
std::cout << "Pool full frees: " << stats.pool_full_frees << "\n"; // Released to OS

// Reset for fresh measurement
cas::message_pool::reset_stats();
```

## Timers

Schedule messages for future delivery:

### One-Shot Timer

```cpp
void on_start() override {
    handler<timeout>(&my_actor::on_timeout);

    // Fire once after 5 seconds
    schedule_once<timeout>(std::chrono::seconds(5));

    // With custom message content
    timeout msg;
    msg.reason = "idle";
    schedule_once(std::chrono::seconds(30), msg);
}

void on_timeout(const timeout& msg) {
    std::cout << "Timer fired!\n";
}
```

### Periodic Timer

```cpp
void on_start() override {
    handler<heartbeat>(&my_actor::on_heartbeat);

    // Fire every 100ms
    m_timer_id = schedule_periodic<heartbeat>(std::chrono::milliseconds(100));
}

void on_heartbeat(const heartbeat& msg) {
    // Called every 100ms
}

void on_shutdown() override {
    // Cancel timer during shutdown
    cancel_timer(m_timer_id);
}

uint64_t m_timer_id;
```

### Timer Cancellation

```cpp
// Cancel by ID
uint64_t id = schedule_periodic<tick>(std::chrono::seconds(1));
cancel_timer(id);

// Timers are automatically cancelled when actor stops
```

## Ask Pattern (RPC)

Make synchronous request-response calls:

```cpp
// Define an operation tag
struct get_balance_op {};

// Server actor
class account_actor : public cas::actor {
protected:
    void on_start() override {
        set_name("account");

        // Register ask handler: returns double, tagged with get_balance_op
        ask_handler<double, get_balance_op>(&account_actor::get_balance);
    }

    double get_balance(const std::string& account_id) {
        return m_balances[account_id];
    }

private:
    std::map<std::string, double> m_balances;
};

// Client code
auto account = cas::actor_registry::get("account");

// Synchronous call with 5 second timeout
auto result = account.ask<double, get_balance_op>(
    std::string("ACC123"),
    std::chrono::seconds(5)
);

if (result) {
    std::cout << "Balance: " << *result << "\n";
} else {
    std::cout << "Request timed out\n";
}
```

**Warning**: Don't call `ask()` from within an actor's message handler - it blocks and can cause deadlocks. Use fire-and-forget `tell()` with correlation IDs instead.

## Actor Types

### Pooled Actors (Default)

Share a thread pool. Good for most use cases.

```cpp
class normal_actor : public cas::actor {
    // ...
};

auto ref = cas::system::create<normal_actor>();
```

### Fast Actors

Dedicated thread per actor. Use for latency-critical paths.

```cpp
class latency_critical : public cas::fast_actor {
    // Same API as regular actor
};

auto ref = cas::system::create<latency_critical>();
// This actor gets its own dedicated thread
```

### Inline Actors

Execute in caller's thread. Zero latency but blocks caller.

```cpp
class sync_processor : public cas::inline_actor {
    // Handlers run synchronously when tell() is called
};

auto ref = cas::system::create<sync_processor>();
ref.tell(msg);  // Blocks until handler completes
```

### Comparison

| Type | Threading | Latency | Use Case |
|------|-----------|---------|----------|
| `cas::actor` | Shared pool | Low (~200ns) | General purpose |
| `cas::fast_actor` | Dedicated thread | Ultra-low (<100ns) | Trading, real-time |
| `cas::inline_actor` | Caller's thread | Zero | Synchronous transforms |

## Common Patterns

### Supervision

```cpp
class supervisor : public cas::actor {
    std::vector<cas::actor_ref> m_workers;

protected:
    void on_start() override {
        set_name("supervisor");
        handler<cas::termination_msg>(&supervisor::on_worker_died);

        // Create workers
        for (int i = 0; i < 4; i++) {
            auto worker = cas::system::create<worker_actor>();
            cas::system::watch(self(), worker);  // Watch for termination
            m_workers.push_back(worker);
        }
    }

    void on_worker_died(const cas::termination_msg& msg) {
        std::cout << "Worker " << msg.actor_name << " died, restarting...\n";
        auto new_worker = cas::system::create<worker_actor>();
        cas::system::watch(self(), new_worker);
        // Replace in vector...
    }
};
```

### Request-Response with Correlation

```cpp
struct request : cas::message_base {
    uint64_t request_id;
    std::string query;
};

struct response : cas::message_base {
    uint64_t request_id;  // Matches request
    std::string result;
};

class client : public cas::actor {
    uint64_t m_next_id = 1;
    std::map<uint64_t, std::function<void(const response&)>> m_pending;

public:
    void send_request(actor_ref server, const std::string& query,
                      std::function<void(const response&)> callback) {
        request req;
        req.request_id = m_next_id++;
        req.query = query;
        m_pending[req.request_id] = callback;
        server.tell(req);
    }

protected:
    void on_start() override {
        handler<response>(&client::on_response);
    }

    void on_response(const response& msg) {
        auto it = m_pending.find(msg.request_id);
        if (it != m_pending.end()) {
            it->second(msg);
            m_pending.erase(it);
        }
    }
};
```

### Load Balancing

```cpp
class load_balancer : public cas::actor {
    std::vector<cas::actor_ref> m_workers;
    size_t m_next = 0;

protected:
    void on_start() override {
        set_name("balancer");
        handler<work_item>(&load_balancer::on_work);

        for (int i = 0; i < 8; i++) {
            m_workers.push_back(cas::system::create<worker_actor>());
        }
    }

    void on_work(const work_item& msg) {
        // Round-robin distribution
        m_workers[m_next++ % m_workers.size()].tell(msg);
    }
};
```

## Troubleshooting

### Messages Not Being Received

1. **Actor not started**: Ensure `cas::system::start()` is called
2. **Handler not registered**: Check `handler<T>()` is called in `on_start()`
3. **Wrong message type**: Handler type must exactly match sent message type
4. **Actor stopped**: Check `ref.is_running()` before sending

### Deadlocks

1. **Don't use `ask()` from within handlers** - Use `tell()` with callbacks
2. **Don't call `wait_for_shutdown()` from an actor** - Only from main thread

### Memory Growing

1. **Pool unlimited**: Set `cas::message_pool::set_max_pool_size(10000)`
2. **Message queues growing**: Check if consumers are slower than producers
3. **Large messages**: Messages >1KB bypass pool, use heap directly

### Performance Issues

1. **Use Release build**: Debug builds are 10x slower
2. **Use `fixed_string`**: Avoid `std::string` in hot-path messages
3. **Pre-warm pool**: Call `cas::message_pool::prewarm()` at startup
4. **Use `fast_actor`**: For latency-critical actors

## Benchmarks

Measured on Windows (MSVC 2022, Release build, single producer/consumer):

| Test | Throughput | Avg Latency |
|------|------------|-------------|
| Minimal message | 5.9 M msg/sec | ~170 ns |
| `std::string` fields | 4.3 M msg/sec | ~234 ns |
| `fixed_string` fields | 4.6 M msg/sec | ~216 ns |

Run `./build/benchmark` to measure on your system.

## API Reference

### cas::system

```cpp
// Create an actor (call before or after start)
template<typename T> static actor_ref create();

// Start the system (call once)
static void start();

// Initiate shutdown (non-blocking)
static void shutdown();

// Wait for all actors to stop (blocking)
static void wait_for_shutdown();

// Stop a specific actor
static void stop_actor(actor_ref ref);
static void stop_actor(const std::string& name);

// Watch for actor termination
static void watch(actor_ref watcher, actor_ref target);
```

### cas::actor

```cpp
// Lifecycle hooks (override these)
virtual void on_start();     // Called when actor starts
virtual void on_shutdown();  // Called during shutdown (can send messages)
virtual void on_stop();      // Called after shutdown (no messages)

// Handler registration (call in on_start)
template<typename T> void handler(void (Derived::*method)(const T&));
template<typename R, typename Tag, typename... Args>
void ask_handler(R (Derived::*method)(Args...));

// Timer scheduling
template<typename T> uint64_t schedule_once(duration delay);
template<typename T> uint64_t schedule_periodic(duration interval);
void cancel_timer(uint64_t timer_id);

// Identity
void set_name(const std::string& name);
actor_ref self();
```

### cas::actor_ref

```cpp
// Send a message (non-blocking)
template<typename T> void tell(const T& msg) const;

// Synchronous call (blocking, with timeout)
template<typename R, typename Tag, typename... Args>
std::optional<R> ask(Args&&... args, duration timeout);

// State queries
bool is_valid() const;    // Points to an actor
bool is_running() const;  // Actor is active
std::string name() const; // Actor's registered name
```

### cas::actor_registry

```cpp
// Look up actor by name
static actor_ref get(const std::string& name);

// Check if name exists
static bool exists(const std::string& name);

// Get count of registered actors
static size_t count();
```

### cas::message_pool

```cpp
// Configure pool size (0 = unlimited)
static void set_max_pool_size(size_t max_blocks_per_class);
static size_t get_max_pool_size();

// Pre-allocate blocks
static void prewarm(size_t count_per_class = 256);

// Statistics
static stats get_stats();
static void reset_stats();
```

## License

[MIT License](LICENSE)

## Contributing

Contributions welcome! Please follow the existing code style (snake_case, m_ prefix for members) and include tests for new features.
