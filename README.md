# Cygnus Actor Framework

A modern C++20 actor framework built for **speed without sacrificing simplicity**.

Most actor frameworks force you to choose: either raw performance with complex APIs, or clean syntax with overhead. Cygnus was designed to deliver both - **8+ million messages/sec from a single sender, 18 million with actors messaging concurrently, and ~1 µs round trips** - with an API that feels natural to C++ developers and can be learned in an afternoon.

### Why Cygnus?

- **Fast**: lock-free mailboxes and scheduling, pooled message allocation.
  8 M msg/sec from one sender and 18 M with 8 actors messaging 8 others;
  round trips ~0.7 µs typical, ~3 µs at p99.9 (MSVC, Ryzen 9 5900X -
  [benchmarks](#benchmarks), with ranges and caveats - run it yourself)
- **Intuitive API**: Define actors in ~10 lines. No boilerplate, no macros, no inheritance hierarchies
- **Easy Learning Curve**: If you know C++ lambdas and inheritance, you already know 80% of Cygnus
- **Production Ready**: Timer scheduling, RPC-style call, actor supervision, dead letter handling

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
- [Configuration](#configuration)
- [Messages](#messages)
- [Actors](#actors)
- [Actor Communication](#actor-communication)
- [Memory Pool](#memory-pool)
- [Timers](#timers)
- [Call (RPC)](#call-rpc)
- [Actor Types](#actor-types)
- [Common Patterns](#common-patterns)
- [Troubleshooting](#troubleshooting)
- [Benchmarks](#benchmarks)
- [API Reference](#api-reference)

**Beyond this page:**

- **[Tutorials](tutorials/)** - fourteen runnable, self-checking programs, in
  teaching order. Every one is compiled and executed by `ctest`, so unlike
  prose they cannot silently go stale. Start at `tutorials 1`.
- **[Full documentation](doc/)** - reference chapters on messaging, lifecycle,
  the registry, call, timers, dynamic removal, advanced actor types and the
  message pool.
- **[Threading invariants](doc/INVARIANTS.md)** - read before changing the
  threading model.

## Installation

### Requirements
- C++20 compiler (MSVC 2019 16.11+, GCC 13+, Clang 16+)
- CMake 3.15+

### Building

```bash
# Clone the repository
git clone https://github.com/cygnussystems/cygnus-actors-cpp.git
cd cygnus-actors-cpp

# Configure and build.
# ZeroMQ support is ON by default and needs cppzmq installed; turn it off if
# you do not have it, otherwise configuration fails looking for the package.
cmake -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_ZEROMQ=OFF
cmake --build build

# Run tests
./build/unit_tests

# Run benchmarks
./build/benchmark
```

On Windows, run these from a Developer Command Prompt (or after `vcvars64.bat`)
so the MSVC toolchain and Windows SDK are on PATH. Configuring outside one
fails while testing the compiler, typically `cannot open file 'kernel32.lib'`.

To build *with* the ZeroMQ relay, install cppzmq first (vcpkg: `cppzmq`) and
drop `-DENABLE_ZEROMQ=OFF`, pointing CMake at the vcpkg toolchain file.

### Integration

Add to your CMakeLists.txt:
```cmake
add_subdirectory(cygnus-actors-cpp)
target_link_libraries(your_target CygnusActorFramework)
```

Linking the target is the supported route: it propagates the C++20 requirement
to your build. If you instead add the headers and sources to your project
directly, compile them as C++20 - the public headers use concepts,
`operator<=>` and `std::jthread`.

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

1. **Create actors before or after `start()`** - after start, `create()` runs the actor's `on_start()` before returning, so it is ready at once
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
    // shutdown() blocks until the queues drain (or drain_timeout expires):
    // - Each actor receives system_shutdown behind the messages already queued
    // - When it handles it: on_shutdown() runs, its timers are cancelled, and
    //   from then on messages to it are dead letters
    // - Messages still queued at drain_timeout are never handled; they are
    //   listed in cas::system::get_shutdown_log()

    // Phase 5: Wait for completion
    cas::system::wait_for_shutdown();
    // Workers joined, on_stop() has run for every actor - safe to exit

    return 0;
}
```

## Configuration

Pass a `cas::system_config` **before creating the first actor** - the pool
width and queue threshold are read when actors are created:

```cpp
cas::system_config cfg;
cfg.thread_pool_size = 4;                             // 0 = hardware concurrency
cfg.max_messages_per_turn = 64;                       // per pooled actor turn; 0 = unlimited
cfg.worker_idle_spin = std::chrono::microseconds(20); // 0 = park at once
cfg.queue_threshold = 1000;                           // queue-size alert; 0 = off
cfg.log_dead_letters = false;                         // also log dead letters to stderr
cas::system::init(cfg);  // or configure(cfg), which throws if the system is running

auto a = cas::system::create<my_actor>();
cas::system::start();
```

- **`max_messages_per_turn`** (default 64): a pooled actor is put on its
  worker's ready queue when a message finds it idle, and handles up to this
  many messages before other ready actors on that worker get a turn. Higher
  favours a busy actor's throughput; lower bounds how long its neighbours wait.
- **`worker_idle_spin`** (default 20 µs): how long an idle pooled worker keeps
  polling before it parks. Work arriving within the spin needs no OS wake-up,
  which keeps pooled round trips around 1 µs. The cost is CPU: roughly spin
  length x bursts of work per second on each busy worker (20 µs x 1000/s = 2%
  of a core); idle workers do not spin.
- **`queue_threshold`** (default 1000): when an actor's queue exceeds it, the
  handler set with `cas::system::set_queue_threshold_handler()` is called on the
  sending thread - once per breach; it re-arms when the queue is back to half
  the threshold. (`cas::queue_threshold_warning` is deprecated: it was never
  sent.)
- `ask_thread_pool_size` is deprecated and ignored.

### Exceptions in handlers

An exception escaping a message handler of a pooled, fast or stateful actor is
caught, counted (`cas::system::handler_exception_count()`) and reported to the
callback set with `cas::system::set_handler_exception_handler()` - or written
to stderr if none is set - and the actor handles its next message. An actor can
override `on_handler_exception()` to return `cas::exception_action::stop`
instead: it is then stopped and its watchers get a `termination_msg`. `call()`
handler exceptions go to the caller; `inline_actor` handler exceptions
propagate to the sender. See [Best Practices](doc/120_best_practices.md).

## Messages

Messages are structs that inherit from `cas::message_base`:

```cpp
struct order : cas::message_base {
    std::string symbol;
    int quantity;
    double price;
};
```

`message_base` is polymorphic (it has a virtual destructor), so messages are
not aggregates: `order{"AAPL", 100, 1.5}` does not compile. Construct, set the
fields, then send:

```cpp
order o;
o.symbol = "AAPL";
o.quantity = 100;
o.price = 1.5;
exchange.tell(o);

exchange.tell(heartbeat{});  // empty braces are fine
```

### Message Fields (Automatic)

Every message automatically has:
```cpp
struct message_base {
    actor_ref sender;        // Who sent this (set by framework)
    uint64_t message_id;     // Unique ID (set by tell()/call() and per timer firing)
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

The registry holds weak references: it never keeps an actor alive, and a
destroyed actor's name reads as absent. Calling `set_name()` again releases the
old name.

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
    log_message msg;              // messages are not aggregates: set fields,
    msg.text = "Something happened";  // then send (see "Messages" above)
    logger.tell(msg);
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
worker.tell(some_message{});  // Dropped, and counted as a dead letter

// The actor_ref is still valid (points to actor object)
worker.is_valid();    // true - object exists
worker.is_running();  // false - actor is stopped

// Useful for a decision, not a guarantee: the actor can stop between the
// check and the tell()
if (worker.is_running()) {
    worker.tell(msg);
} else {
    // Handle actor being down
}
```

**Important**: a message to a stopped actor - or sent through an invalid
`actor_ref` (counted in `dropped_to_invalid`) - is dropped without an exception.
It is counted as a dead letter (`cas::system::get_dead_letter_stats()`), passed
to your dead-letter handler if you set one
(`cas::system::set_dead_letter_handler()`), and logged if
`system_config::log_dead_letters` is on. If delivery must be confirmed, have
the receiver reply.

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
│    2. Pop block from this thread's cache (fast path)        │
│    3. If empty, refill 16 from the lock-free shared list    │
│    4. If that is empty too, allocate from heap (slow path)  │
│                                                              │
│  Deallocation:                                               │
│    1. Push block to this thread's cache                     │
│    2. If full, move 16 to the shared list (if under limit)  │
│    3. Or free to OS (if pool is full)                       │
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

For messages that never touch the heap, use `cas::fixed_string<N>`:

```cpp
// std::string may allocate, and the message is not self-contained
struct slow_message : cas::message_base {
    std::string symbol;      // heap once it exceeds the SSO buffer
    std::string client_id;   // heap once it exceeds the SSO buffer
};

// GOOD for HFT: fixed_string is inline
struct fast_message : cas::message_base {
    cas::fixed_string<8> symbol;      // Inline, never allocates
    cas::fixed_string<16> client_id;  // Inline, never allocates
    int64_t quantity;
    int64_t price;
};
```

**Be honest about when this wins.** Both major standard libraries keep short
strings inline (the small-string optimisation - roughly 15 characters), so for
values that short `std::string` does not allocate either. Even so, in the
[benchmarks](#benchmarks) `fixed_string` fields are about 2x faster than
`std::string` fields (see the notes there). `fixed_string` also earns its place
when values are longer than the SSO buffer, when you need a message whose size
is fixed and known at compile time, or when you want the guarantee rather than
a heuristic that depends on your standard library.

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
// Either operand order, and against any capacity.
if (str == "hello") { }
if ("hello" == str) { }
if (str < other_str) { }             // other_str may be any fixed_string<N>

// Usable as a key in unordered containers. Hashes consistently with
// std::string_view, so equal contents always hash equally.
std::unordered_map<cas::fixed_string<8>, order> by_symbol;

// Conversion
std::string s = str.str();           // Copy to std::string
std::string_view sv = str.view();    // View (no copy)
std::string_view sv2 = str;          // Implicit conversion

// Compile-time construction and comparison
static_assert(cas::fixed_string<8>("AAPL") == "AAPL");
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
    schedule_once(std::chrono::seconds(5), timeout{});

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
    m_timer_id = schedule_periodic(std::chrono::milliseconds(100), heartbeat{});
}

void on_heartbeat(const heartbeat& msg) {
    // Called every 100ms
}

void on_shutdown() override {
    // Nothing to do: the actor's timers are cancelled once on_shutdown()
    // returns (optional: cancel_timer(m_timer_id) here)
}

cas::timer_id m_timer_id = cas::INVALID_TIMER_ID;
```

### Timer Cancellation

```cpp
// Cancel by ID
cas::timer_id id = schedule_periodic(std::chrono::seconds(1), tick{});
cancel_timer(id);

// An actor's timers are cancelled once its on_shutdown() has run, whether it
// was stopped with stop_actor() or by system shutdown. While stopping, an
// actor cannot schedule new timers (schedule_* returns INVALID_TIMER_ID).
// Each firing delivers a copy of the message with its own message_id.
```

## Call (RPC)

Make synchronous request-response calls:

```cpp
// Define an operation tag
struct get_balance_op {};

// Server actor
class account_actor : public cas::actor {
protected:
    void on_start() override {
        set_name("account");

        // Register call handler: returns double, tagged with get_balance_op
        call_handler<double, get_balance_op>(&account_actor::get_balance);
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
auto result = account.call<double>(
    get_balance_op{},
    std::chrono::seconds(5),
    std::string("ACC123")   // exact parameter type: a "ACC123" literal would be rejected
);

if (result) {
    std::cout << "Balance: " << *result << "\n";
} else {
    std::cout << "Request timed out\n";
}
```

A call is handled **next** by the target - ahead of messages already queued - so
it gets an answer even from an actor that is behind (see
[doc/80_call.md](doc/80_call.md)).

**Warning**: `call()` blocks the calling thread. From inside an actor's handler
that stalls every actor on the same worker until the reply arrives, and calling
an actor that only your thread runs (one on your own worker) throws
`call_deadlock_error`. Between actors,
prefer `tell()` with a reply message.

`call()` was named `ask()` in 1.0; the old names still compile, with a
deprecation warning.

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
// inline_actor<> = thread-safe (mutex); inline_actor<false> = single sender, no mutex
class sync_processor : public cas::inline_actor<> {
    // Handlers run synchronously when tell() is called
};

auto ref = cas::system::create<sync_processor>();
ref.tell(msg);  // Blocks until handler completes
```

A handler may `tell()` its own inline actor: the message is handled right after
the current handler, before the outer `tell()` returns. A `call()` back into an
inline actor that is mid-handler throws `call_deadlock_error`.

### Stateful Actors

Selective receive for state machines: in each state the actor accepts only some
message types; the others stay queued, in order, until a later state accepts
them. Calls and system shutdown are never held back.

```cpp
struct login : cas::message_base { cas::fixed_string<16> user; };
struct new_order : cas::message_base { int quantity = 0; };

class session : public cas::stateful_actor {  // or cas::stateful_fast_actor
protected:
    void on_start() override {
        handler<login>(&session::on_login);
        handler<new_order>(&session::on_order);
        reject_all_message_types();
        accept_message_type<login>();   // orders wait until logged in
    }

    void on_login(const login&) { accept_all_message_types(); }
    void on_order(const new_order&) { /* ... */ }
};
```

`stateful_actor` runs on the pool; `stateful_fast_actor` has the same API on a
dedicated thread. Call `accept_*`/`reject_*` only from the actor's own
`on_start()` or handlers.

### Comparison

| Type | Threading | Round trip (p50, MSVC) | Use Case |
|------|-----------|------------------------|----------|
| `cas::actor` | Shared pool | ~0.7 µs | General purpose |
| `cas::fast_actor` | Dedicated thread | ~0.5 µs, no worker sharing | Trading, real-time |
| `cas::inline_actor` | Caller's thread | None (no queue) | Synchronous transforms |
| `cas::stateful_actor` | Shared pool | as `cas::actor` | State machines (selective receive) |
| `cas::stateful_fast_actor` | Dedicated thread | as `cas::fast_actor` | Latency-critical state machines |

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

1. **Don't use `call()` from within handlers** - Use `tell()` with callbacks
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

Release builds, measured on an **AMD Ryzen 9 5900X** (desktop, 12 cores / 24
threads). Figures are the median of five runs, with the observed range in
brackets. Two programs: `benchmark` (throughput) and `latency_benchmark`
(round-trip percentiles).

**Throughput** - 1M messages per single-sender case, 4M for the actor case:

| Test | MSVC 2022 (Windows) | GCC 13 (WSL Ubuntu) |
|------|---------------------|---------------------|
| Minimal message, one sender | 8.0 M msg/sec [7.97–9.17] | 12.7 M msg/sec [12.40–13.71] |
| `std::string` fields, one sender | 3.4 M msg/sec [3.27–3.43] | 5.7 M msg/sec [5.60–5.76] |
| `fixed_string` fields, one sender | 8.0 M msg/sec [6.43–7.99] | 11.4 M msg/sec [10.89–13.24] |
| 8 actors → 8 actors, concurrently | 18.2 M msg/sec [15.89–18.32] | 4.8 M msg/sec [4.66–5.27] * |

**Latency** - ping-pong round trip between two actors, one message in flight:

| Path | MSVC 2022: p50 / p99 / p99.9 | GCC 13 (WSL): p50 / p99 / p99.9 |
|------|------------------------------|---------------------------------|
| pooled → pooled | 0.7 / 1.7 / 3.7 µs | 0.6 / 1.3 / 14.5 µs |
| `fast_actor` → `fast_actor` | 0.5 / 1.3 / 3.1 µs | 1.1 / 2.3 / 22.2 µs |
| pooled → `fast_actor` | 0.5 / 1.4 / 2.6 µs | 1.1 / 1.7 / 20.2 µs |

Worst-case (max) samples are in the hundreds of microseconds on both: that is
operating-system scheduling on a desktop, not the framework. For
latency-critical work, pin threads and isolate cores.

Worth reading off these tables rather than glossing over:

- **\* WSL sees 6 of the 24 hardware threads**, so the 8-to-8 actor case runs
  16 busy actors on 6 CPUs there. Compare compilers on the single-sender rows.
- **`std::string` is the one case that did not improve** with the new scheduler
  (MSVC: ~4.0 → 3.4 M against 1.0; `fixed_string`, same fields, went 3.8 → 8.0).
  Each copied `std::string` allocates on the sender's thread and frees on the
  receiver's; with a much faster receiver the two now run concurrently on the
  heap. That explanation is a hypothesis, not yet profiled. Either way:
  keep heap-allocating members off hot paths - use `fixed_string` or a
  shared payload (see `doc/140_message_pool.md`).
- **`fixed_string` vs `std::string` depends on the library.** The benchmark's
  symbol is short enough for the small-string optimisation, so part of that
  comparison measures copying, not allocation.

1.0.0 measured 4.6 M msg/sec (minimal, MSVC) on a Turbo-disabled 6-core laptop;
the same code on this desktop measured ~4.7 M, and the concurrent 8-to-8 case
~2.8 M - the scheduler rewrite is where most of the difference above comes from.

Run `./build/benchmark` and `./build/latency_benchmark` to measure on your
system. Both take an optional argument: pooled workers' idle spin in
microseconds (`system_config::worker_idle_spin`).

## API Reference

### cas::system

```cpp
// Configure (before the first create())
static void init(const system_config& config = {});
static void configure(const system_config& config);  // throws if running

// Create an actor (call before or after start); args go to T's constructor
template<typename T, typename... Args> static actor_ref create(Args&&... args);

// Start the system (call once)
static void start();

// Initiate shutdown; blocks until queues drain or config.drain_timeout
// (default 5 s) expires
static void shutdown(const shutdown_config& config = {});

// Wait for all actors to stop (blocking)
static void wait_for_shutdown();
static std::vector<std::string> get_shutdown_log();  // e.g. actors left with messages

// Stop a specific actor
static bool stop_actor(actor_ref ref, const stop_config& config = {});
static bool stop_actor(const std::string& name, const stop_config& config = {});
static bool is_actor_running(actor_ref ref);

// Watch for actor termination
static void watch(actor_ref watcher, actor_ref target);
static void unwatch(actor_ref watcher, actor_ref target);

// Dead letters (dropped_tell, dropped_ask, dropped_to_invalid)
static dead_letter_stats get_dead_letter_stats();
static void reset_dead_letter_stats();
static void set_dead_letter_handler(dead_letter_handler handler);
static void clear_dead_letter_handler();

// Queue threshold alerts (once per breach, re-armed at half the threshold)
static void set_queue_threshold_handler(queue_threshold_handler handler);
static void clear_queue_threshold_handler();

// Exceptions escaping handlers / on_shutdown / on_stop (caught, counted, reported)
static void set_handler_exception_handler(handler_exception_handler handler);
static void clear_handler_exception_handler();
static size_t handler_exception_count();
```

### cas::actor

```cpp
// Lifecycle hooks (override these)
virtual void on_start() = 0; // Required: called once when actor starts
virtual void on_shutdown();  // Called during shutdown (can send messages)
virtual void on_stop();      // Called after shutdown (no messages)
// A handler threw (caught by the framework): resume (default) or stop this actor
virtual cas::exception_action on_handler_exception(const cas::message_base& msg, std::exception_ptr error);

// Handler registration (call in on_start)
// T must derive from cas::message_base; a type that cannot reach a mailbox
// is rejected at the registration call, not silently left unreachable.
template<typename T> void handler(void (Derived::*method)(const T&));
template<typename T, typename Callable> void handler(Callable&& callable);
template<typename R, typename Tag, typename... Args>
void call_handler(R (Derived::*method)(Args...));

// Timer scheduling (the message is copied; each firing delivers a copy with its own message_id)
template<typename T> cas::timer_id schedule_once(std::chrono::milliseconds delay, T msg);
template<typename T> cas::timer_id schedule_periodic(std::chrono::milliseconds interval, T msg);
void cancel_timer(cas::timer_id id);

// Identity
void set_name(const std::string& name);   // releases any previous registry name
const std::string& name() const;
actor_ref self();

// Queue metrics
size_t mailbox_high_water_mark() const;
size_t call_queue_high_water_mark() const;
size_t total_high_water_mark() const;     // largest combined queue seen
```

### cas::actor_ref

```cpp
// Send a message (non-blocking)
template<typename T> void tell(const T& msg) const;

// Synchronous call (blocking). Argument types must match the handler's
// parameter types exactly; a mismatch throws cas::call_type_error.
template<typename R, typename Tag, typename... Args>
R call(Tag op, Args&&... args);
template<typename R, typename Tag, typename... Args>
std::optional<R> call(Tag op, std::chrono::milliseconds timeout, Args&&... args);

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
