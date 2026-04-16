# Advanced Actor Types

Cygnus provides three specialized actor types beyond the standard `cas::actor` for specific performance and concurrency scenarios.

## Quick Comparison

| Actor Type | Thread Model | Latency | CPU Usage | Thread Safety | Use Case |
|------------|--------------|---------|-----------|---------------|----------|
| **Pooled** (default) | Thread pool | ~10μs | Low | Automatic | General purpose |
| **Fast Actor** | Dedicated | <1μs | High | Automatic | Real-time systems |
| **Inline Actor** | Caller's thread | 0μs | Zero overhead | Configurable | Synchronous services |
| **Stateful Actor** | Thread pool | ~10μs | Low | Automatic | State machines |

## Fast Actors

Fast actors run on dedicated threads with tight polling loops for ultra-low latency message processing.

### When to Use Fast Actors

**Use fast actors when:**
- You need sub-microsecond latency
- Actor processes high-frequency messages (thousands/sec)
- Dedicated CPU core is acceptable
- Examples: Trading systems, game loops, real-time control

**Don't use fast actors when:**
- You have many actors (use pooled actors instead)
- Latency requirements are relaxed (>1ms acceptable)
- CPU resources are limited

### Creating Fast Actors

```cpp
#include <cas/fast_actor.h>

class trading_engine : public cas::fast_actor {
protected:
    void on_start() override {
        set_name("engine");
        handler<market_tick>(&trading_engine::on_tick);

        // Set polling strategy
        set_polling_strategy(cas::polling_strategy::hybrid);
        set_spin_count(100);  // Spin 100 iterations before yielding
    }

    void on_tick(const market_tick& msg) {
        // Process with ultra-low latency
        execute_orders(msg);
    }
};

// Create and start
auto engine = cas::system::create<trading_engine>();
cas::system::start();
```

### Polling Strategies

Fast actors support three polling strategies:

**1. Yield (Default) - Cooperative CPU Usage**
```cpp
class low_latency_actor : public cas::fast_actor {
public:
    low_latency_actor() : cas::fast_actor(cas::polling_strategy::yield) {}
};
```

- **Latency**: ~1-10μs
- **CPU**: Cooperative (yields to other threads)
- **When**: Most fast actor use cases, balanced performance

**2. Hybrid - Ultra-Low Latency**
```cpp
class ultra_fast_actor : public cas::fast_actor {
protected:
    void on_start() override {
        set_polling_strategy(cas::polling_strategy::hybrid);
        set_spin_count(100);  // Spin iterations before yield
    }
};
```

- **Latency**: <1μs
- **CPU**: Moderate (spins briefly, then yields)
- **When**: Critical latency paths, moderate message frequency

**3. Busy Wait - Minimum Latency**
```cpp
class realtime_actor : public cas::fast_actor {
public:
    realtime_actor() : cas::fast_actor(cas::polling_strategy::busy_wait) {}
};
```

- **Latency**: <100ns (theoretical minimum)
- **CPU**: 100% of dedicated core
- **When**: Absolute minimum latency required, dedicated core available

### Fast Actor Example: Game Loop

```cpp
struct frame_tick : public cas::message_base {
    std::chrono::steady_clock::time_point timestamp;
};

class game_loop : public cas::fast_actor {
private:
    int m_frame_count = 0;
    std::chrono::microseconds m_total_frame_time{0};

protected:
    void on_start() override {
        set_name("game_loop");
        handler<frame_tick>(&game_loop::on_frame);

        // Use hybrid for 60 FPS target with low latency
        set_polling_strategy(cas::polling_strategy::hybrid);
        set_spin_count(50);
    }

    void on_frame(const frame_tick& msg) {
        auto now = std::chrono::steady_clock::now();
        auto frame_time = std::chrono::duration_cast<std::chrono::microseconds>(
            now - msg.timestamp);

        m_frame_count++;
        m_total_frame_time += frame_time;

        // Process game logic
        update_game_state();
        render_frame();
    }

public:
    double avg_frame_time_ms() const {
        if (m_frame_count == 0) return 0.0;
        return m_total_frame_time.count() / static_cast<double>(m_frame_count) / 1000.0;
    }
};

// Usage
auto game = cas::system::create<game_loop>();
cas::system::start();

auto game_ref = cas::actor_registry::get("game_loop");

// Send frame ticks at 60 FPS
for (int i = 0; i < 60; ++i) {
    frame_tick tick;
    tick.timestamp = std::chrono::steady_clock::now();
    game_ref.tell(tick);
    std::this_thread::sleep_for(std::chrono::milliseconds(16));  // ~60 FPS
}
```

### Fast Actor Performance Considerations

**Advantages:**
- Dedicated thread eliminates thread switching overhead
- Tight polling loop minimizes latency
- Configurable trade-off between latency and CPU usage
- Predictable, consistent performance

**Trade-offs:**
- One thread per fast actor (resource intensive)
- Higher CPU usage even when idle (depending on strategy)
- Not suitable for large numbers of actors
- Shutdown requires thread join (slightly slower)

**Best Practices:**
```cpp
// ✓ GOOD: Use for high-frequency, latency-critical actors
class market_data_feed : public cas::fast_actor {
    // Processes thousands of ticks per second
};

// ✗ BAD: Don't use for occasional messages
class email_sender : public cas::fast_actor {  // Overkill!
    // Sends a few emails per minute
};

// ✓ GOOD: Limit number of fast actors
// Create 1-5 fast actors for critical paths
auto engine1 = cas::system::create<trading_engine>();
auto engine2 = cas::system::create<risk_calculator>();

// ✗ BAD: Don't create hundreds of fast actors
for (int i = 0; i < 1000; ++i) {  // Too many threads!
    auto worker = cas::system::create<fast_worker>();
}
```

## Inline Actors

Inline actors process messages synchronously in the caller's thread with zero queuing latency.

### When to Use Inline Actors

**Use inline actors when:**
- You need synchronous execution (no queuing)
- Zero latency is critical
- Direct method calls are preferred
- Actor is called from single thread or needs thread-safe variant
- Examples: Calculators, validators, synchronous services

**Don't use inline actors when:**
- You need asynchronous message processing
- Caller should not block
- Actor has long-running operations

### Creating Inline Actors

**Thread-Safe Variant (Default):**
```cpp
#include <cas/inline_actor.h>

// Thread-safe: Can be called from multiple threads
class calculator : public cas::inline_actor<true> {
private:
    int m_total = 0;

protected:
    void on_start() override {
        set_name("calculator");
        handler<add>(&calculator::on_add);
    }

    void on_add(const add& msg) {
        m_total += msg.value;
    }

public:
    // Direct method access (add your own mutex if needed)
    int calculate(int a, int b) const {
        return a + b;
    }

    int total() const {
        // Note: Message handlers are thread-safe, but direct methods need own protection
        return m_total;
    }
};
```

**Non-Thread-Safe Variant (Fastest):**
```cpp
// Non-thread-safe: Single caller only, no mutex overhead
class fast_validator : public cas::inline_actor<false> {
private:
    int m_validation_count = 0;

protected:
    void on_start() override {
        set_name("validator");
    }

public:
    bool validate(const std::string& input) {
        m_validation_count++;
        return input.length() > 0 && input.length() <= 100;
    }

    int validation_count() const { return m_validation_count; }
};
```

### Inline Actor Usage Patterns

**Pattern 1: Direct Method Calls (Recommended)**
```cpp
auto calc = cas::system::create<calculator>();
cas::system::start();

// Direct method call - zero latency, synchronous
auto& calc_actor = calc.get_checked<calculator>();
int result = calc_actor.calculate(21, 21);  // Returns 42 immediately
```

**Pattern 2: Message Passing (Synchronous)**
```cpp
auto calc_ref = cas::actor_registry::get("calculator");

add msg;
msg.value = 10;
calc_ref.tell(msg);  // Processed immediately in this thread, no queuing

// Note: Changes to msg won't be visible (message is copied)
// Use direct method calls for return values
```

**Pattern 3: Multi-Threaded Access (Thread-Safe Variant)**
```cpp
class thread_safe_counter : public cas::inline_actor<true> {
private:
    std::atomic<int> m_count{0};

public:
    void increment() {
        m_count++;
    }

    int count() const { return m_count; }
};

// Multiple threads calling concurrently
auto counter = cas::system::create<thread_safe_counter>();
cas::system::start();

auto& counter_actor = counter.get_checked<thread_safe_counter>();

std::vector<std::thread> threads;
for (int i = 0; i < 10; ++i) {
    threads.emplace_back([&counter_actor]() {
        for (int j = 0; j < 100; ++j) {
            counter_actor.increment();  // Protected by inline_actor's mutex
        }
    });
}

for (auto& t : threads) {
    t.join();
}

// All increments counted correctly
assert(counter_actor.count() == 1000);
```

### Inline Actor Example: Synchronous Service

```cpp
struct validation_request : public cas::message_base {
    std::string data;
    mutable bool valid;  // Result written here
};

class data_validator : public cas::inline_actor<true> {
private:
    std::set<std::string> m_blacklist;

protected:
    void on_start() override {
        set_name("validator");
        handler<validation_request>(&data_validator::on_validate);

        // Load blacklist
        m_blacklist = {"spam", "malicious", "invalid"};
    }

    void on_validate(const validation_request& msg) {
        msg.valid = m_blacklist.find(msg.data) == m_blacklist.end();
    }

public:
    // Direct validation method (preferred)
    bool validate(const std::string& data) const {
        return m_blacklist.find(data) == m_blacklist.end();
    }
};

// Usage
auto validator = cas::system::create<data_validator>();
cas::system::start();

auto& validator_actor = validator.get_checked<data_validator>();

// Direct call - immediate result
bool is_valid = validator_actor.validate("user_input");  // Returns immediately

if (is_valid) {
    process_data();
}
```

### Inline Actor Performance Considerations

**Advantages:**
- Zero queuing latency (immediate execution)
- No thread switching overhead
- Minimal memory footprint (no message queues)
- Simple mental model (synchronous execution)

**Trade-offs:**
- Blocks caller's thread during execution
- Long-running handlers block caller
- Thread-safe variant has mutex overhead
- Non-thread-safe variant requires single caller

**Best Practices:**
```cpp
// ✓ GOOD: Use for quick, synchronous operations
class parser : public cas::inline_actor<true> {
    // Parse small JSON strings, return immediately
};

// ✗ BAD: Don't use for long-running operations
class file_processor : public cas::inline_actor<true> {
    void process_file() {
        // Reading large file blocks caller!
    }
};

// ✓ GOOD: Use non-thread-safe for single-caller scenarios
class session_state : public cas::inline_actor<false> {
    // Only called by owning connection actor
};

// ✓ GOOD: Prefer direct method calls over message passing
auto result = actor.calculate(10);  // Good
actor.tell(calculate_msg);       // Not as good (result lost in copy)
```

## Stateful Actors

Stateful actors selectively process messages based on current state, deferring incompatible messages.

### When to Use Stateful Actors

**Use stateful actors when:**
- Actor behavior depends on internal state
- Messages should be rejected/deferred in certain states
- Implementing state machines or protocols
- Examples: Connection handlers, download managers, protocol implementations

**Don't use stateful actors when:**
- Actor processes all messages regardless of state
- Simple request-response patterns
- State doesn't affect message acceptance

### Creating Stateful Actors

```cpp
#include <cas/stateful_actor.h>

class connection : public cas::stateful_actor {
public:
    enum class state { disconnected, connecting, connected };

private:
    state m_state = state::disconnected;
    std::string m_peer_address;

protected:
    void on_start() override {
        set_name("connection");

        // Register all handlers
        handler<connect>(&connection::on_connect);
        handler<disconnect>(&connection::on_disconnect);
        handler<send_data>(&connection::on_send);
        handler<connection_established>(&connection::on_established);

        // Start in disconnected state
        enter_disconnected_state();
    }

    void enter_disconnected_state() {
        m_state = state::disconnected;

        // Only accept connect messages
        accept_message_type<connect>();
        reject_message_type<disconnect>();
        reject_message_type<send_data>();
        reject_message_type<connection_established>();
    }

    void enter_connecting_state() {
        m_state = state::connecting;

        // Only accept established or cancel messages
        reject_message_type<connect>();
        reject_message_type<disconnect>();  // Queue for later
        reject_message_type<send_data>();   // Queue for later
        accept_message_type<connection_established>();
    }

    void enter_connected_state() {
        m_state = state::connected;

        // Accept disconnect and send_data
        reject_message_type<connect>();
        accept_message_type<disconnect>();
        accept_message_type<send_data>();
        reject_message_type<connection_established>();
    }

    void on_connect(const connect& msg) {
        m_peer_address = msg.address;
        enter_connecting_state();

        // Initiate async connection
        start_connection(msg.address);
    }

    void on_established(const connection_established& msg) {
        enter_connected_state();

        // Process any queued send_data messages
        // (They were deferred during connecting state)
    }

    void on_send(const send_data& msg) {
        // Only called when connected
        transmit(msg.data);
    }

    void on_disconnect(const disconnect& msg) {
        close_connection();
        enter_disconnected_state();
    }
};
```

### Stateful Actor API

**Accept/Reject Message Types:**
```cpp
class my_actor : public cas::stateful_actor {
protected:
    void enter_state_a() {
        // Accept only message_a and message_b
        accept_message_type<message_a>();
        accept_message_type<message_b>();
        reject_message_type<message_c>();
    }

    void enter_state_b() {
        // Accept only message_c
        reject_message_type<message_a>();
        reject_message_type<message_b>();
        accept_message_type<message_c>();
    }

    void accept_everything() {
        accept_all_message_types();  // Default behavior
    }

    void reject_everything() {
        reject_all_message_types();  // Explicit lockout
    }
};
```

**Message Processing Behavior:**
- Accepted messages: Processed immediately
- Rejected messages: Remain queued, processed when type is accepted
- State changes: Trigger re-evaluation of queued messages

### Stateful Actor Example: Download Manager

```cpp
struct start_download : public cas::message_base {
    int file_id;
    std::string url;
};

struct pause_download : public cas::message_base {};
struct resume_download : public cas::message_base {};
struct cancel_download : public cas::message_base {};
struct download_complete : public cas::message_base {};

class download_manager : public cas::stateful_actor {
public:
    enum class state { idle, downloading, paused };

private:
    state m_state = state::idle;
    int m_current_file_id = -1;
    int m_bytes_downloaded = 0;
    int m_downloads_completed = 0;

protected:
    void on_start() override {
        set_name("download_manager");

        handler<start_download>(&download_manager::on_start);
        handler<pause_download>(&download_manager::on_pause);
        handler<resume_download>(&download_manager::on_resume);
        handler<cancel_download>(&download_manager::on_cancel);
        handler<download_complete>(&download_manager::on_complete);

        enter_idle_state();
    }

    void enter_idle_state() {
        m_state = state::idle;

        accept_message_type<start_download>();
        reject_message_type<pause_download>();
        reject_message_type<resume_download>();
        reject_message_type<cancel_download>();
        reject_message_type<download_complete>();
    }

    void enter_downloading_state() {
        m_state = state::downloading;

        reject_message_type<start_download>();  // Queue new downloads
        accept_message_type<pause_download>();
        reject_message_type<resume_download>();
        accept_message_type<cancel_download>();
        accept_message_type<download_complete>();
    }

    void enter_paused_state() {
        m_state = state::paused;

        reject_message_type<start_download>();
        reject_message_type<pause_download>();
        accept_message_type<resume_download>();
        accept_message_type<cancel_download>();
        reject_message_type<download_complete>();
    }

    void on_start(const start_download& msg) {
        m_current_file_id = msg.file_id;
        m_bytes_downloaded = 0;
        enter_downloading_state();

        // Begin download
        initiate_download(msg.url);
    }

    void on_pause(const pause_download& msg) {
        enter_paused_state();
        suspend_download();
    }

    void on_resume(const resume_download& msg) {
        enter_downloading_state();
        continue_download();
    }

    void on_cancel(const cancel_download& msg) {
        abort_download();
        m_current_file_id = -1;
        enter_idle_state();

        // Process next queued start_download if any
    }

    void on_complete(const download_complete& msg) {
        m_downloads_completed++;
        m_current_file_id = -1;
        enter_idle_state();

        // Process next queued start_download if any
    }

public:
    state get_state() const { return m_state; }
    int downloads_completed() const { return m_downloads_completed; }
};

// Usage
auto manager = cas::system::create<download_manager>();
cas::system::start();

auto mgr_ref = cas::actor_registry::get("download_manager");

// Start download
start_download start;
start.file_id = 1;
start.url = "http://example.com/file.zip";
mgr_ref.tell(start);

// Pause it
pause_download pause;
mgr_ref.tell(pause);

// Try to complete while paused - message queued, not processed
download_complete complete;
mgr_ref.tell(complete);  // Queued (rejected in paused state)

// Resume - complete message now processed
resume_download resume;
mgr_ref.tell(resume);  // Enters downloading state, processes queued complete
```

### Deferred Message Processing

When a message is rejected in current state, it remains queued:

```
Timeline:
1. Actor in state A (accepts msg_a, rejects msg_b)
2. Receive msg_b → Queued (not processed)
3. Receive msg_a → Processed immediately
4. Transition to state B (accepts msg_b, rejects msg_a)
5. msg_b automatically processed (now accepted)
```

**Example:**
```cpp
// Actor in IDLE state (only accepts start_download)
mgr_ref.tell(cancel_download{});  // Queued (rejected)
mgr_ref.tell(start_download{});   // Processed (accepted)

// Now in DOWNLOADING state (accepts cancel_download)
// Previously queued cancel_download now processed
```

### Stateful Actor Performance Considerations

**Advantages:**
- Clean state machine implementation
- Automatic message deferral
- Type-safe state transitions
- Prevents invalid message sequences

**Trade-offs:**
- Deque instead of lock-free queue (selective extraction)
- Mutex for accepted types set
- Memory for queued rejected messages
- Slightly higher latency than regular actors

**Best Practices:**
```cpp
// ✓ GOOD: Use for protocol implementations
class tcp_connection : public cas::stateful_actor {
    // States: disconnected, connecting, connected, closing
};

// ✓ GOOD: Use for resource management
class file_handler : public cas::stateful_actor {
    // States: closed, opening, open, reading, writing
};

// ✗ BAD: Don't use when all messages valid in all states
class logger : public cas::stateful_actor {  // Overkill!
    // Logs everything regardless of state
};

// ✓ GOOD: Keep state transition logic clear
void enter_state_x() {
    m_state = state::x;
    accept_message_type<msg_valid_in_x>();
    reject_message_type<msg_not_valid_in_x>();
}

// ✗ BAD: Don't scatter accept/reject calls
void on_some_message() {
    accept_message_type<other_msg>();  // Confusing!
    // Better: transition to well-defined state
}
```

## Choosing the Right Actor Type

### Decision Tree

```
Need synchronous execution (no queuing)?
├─ Yes → Inline Actor
│  ├─ Single caller only? → inline_actor<false>
│  └─ Multiple callers? → inline_actor<true>
│
└─ No → Need asynchronous processing
   ├─ Sub-microsecond latency required?
   │  └─ Yes → Fast Actor
   │     ├─ Minimize latency, dedicated core available? → busy_wait
   │     ├─ Balance latency and CPU? → hybrid
   │     └─ Cooperative CPU usage? → yield
   │
   └─ No → Regular asynchronous actor
      ├─ State-based message filtering needed?
      │  └─ Yes → Stateful Actor
      │
      └─ No → Pooled Actor (default)
```

### Use Case Examples

**High-Frequency Trading System:**
```cpp
// Market data feed - ultra-low latency
class market_feed : public cas::fast_actor {
    // Dedicated thread, hybrid polling
};

// Order validator - synchronous checks
class order_validator : public cas::inline_actor<true> {
    // Direct method calls, zero latency
};

// Risk calculator - fast but not critical path
class risk_engine : public cas::fast_actor {
    // Dedicated thread, yield polling
};

// Order router - state-based routing
class order_router : public cas::stateful_actor {
    // States: market_open, market_closed, halted
};
```

**Game Engine:**
```cpp
// Main game loop - consistent frame timing
class game_loop : public cas::fast_actor {
    // Dedicated thread, hybrid polling
};

// Physics engine - synchronous calculations
class physics : public cas::inline_actor<false> {
    // Single-threaded, direct method calls
};

// Asset loader - state machine
class asset_loader : public cas::stateful_actor {
    // States: idle, loading, loaded
};

// Network sync - regular pooled actor
class network_sync : public cas::actor {
    // Async message handling sufficient
};
```

**Web Server:**
```cpp
// Connection handler - state machine
class http_connection : public cas::stateful_actor {
    // States: reading_request, processing, writing_response
};

// Request validator - synchronous checks
class validator : public cas::inline_actor<true> {
    // Multi-threaded access, direct validation
};

// Logger - regular pooled actor
class logger : public cas::actor {
    // Async logging, no special requirements
};
```

## Performance Comparison

Based on typical workloads:

| Operation | Pooled | Fast (yield) | Fast (hybrid) | Fast (busy) | Inline |
|-----------|--------|--------------|---------------|-------------|--------|
| Message latency | ~10μs | ~1-5μs | ~0.5-1μs | <100ns | 0 (synchronous) |
| CPU usage (idle) | 0% | ~5% | ~20% | 100% | 0% |
| Memory per actor | ~1KB | ~100KB | ~100KB | ~100KB | <1KB |
| Scalability | 1000s | 10s | 10s | 5-10 | 1000s |
| Thread overhead | Shared pool | 1 per actor | 1 per actor | 1 per actor | Caller's thread |

**Notes:**
- Pooled actors share thread pool (default 4-8 threads)
- Fast actors get dedicated thread each
- Inline actors execute in caller's thread (zero threads)
- Stateful actors perform like pooled actors with deque overhead

## Common Patterns

### Pattern: Fast Actor with Inline Helper

```cpp
// Fast actor for critical path
class trading_engine : public cas::fast_actor {
private:
    cas::actor_ref m_risk_calculator;

protected:
    void on_start() override {
        set_polling_strategy(cas::polling_strategy::hybrid);
        handler<order>(&trading_engine::on_order);

        // Create inline actor for synchronous calculations
        auto calc = cas::system::create<risk_calculator>();
        m_risk_calculator = cas::actor_registry::get("risk_calc");
    }

    void on_order(const order& msg) {
        // Synchronous risk check (inline actor)
        auto& calc = m_risk_calculator.get_checked<risk_calculator>();
        if (!calc.check_risk(msg)) {
            reject_order(msg);
            return;
        }

        // Execute order
        execute(msg);
    }
};

class risk_calculator : public cas::inline_actor<true> {
public:
    bool check_risk(const order& o) const {
        // Synchronous calculation, returns immediately
        return o.quantity * o.price <= risk_limit();
    }
};
```

### Pattern: Stateful Actor with State Persistence

```cpp
class persistent_session : public cas::stateful_actor {
private:
    state m_current_state;

protected:
    void on_start() override {
        // Load persisted state
        m_current_state = load_state_from_db();

        // Enter correct state based on persisted data
        switch (m_current_state) {
            case state::idle:
                enter_idle_state();
                break;
            case state::active:
                enter_active_state();
                break;
        }
    }

    void on_shutdown() override {
        // Persist state before shutdown
        save_state_to_db(m_current_state);
    }
};
```

### Pattern: Mixed Actor Types in System

```cpp
// Architecture: Different actors for different needs
void setup_trading_system() {
    // Fast actors for critical path
    auto market_feed = cas::system::create<market_data_feed>();
    auto execution_engine = cas::system::create<execution_engine>();

    // Inline actors for synchronous services
    auto validator = cas::system::create<order_validator>();
    auto calculator = cas::system::create<risk_calculator>();

    // Stateful actors for connection handling
    auto exchange_conn = cas::system::create<exchange_connection>();

    // Regular pooled actors for non-critical tasks
    auto logger = cas::system::create<file_logger>();
    auto metrics = cas::system::create<metrics_collector>();

    cas::system::start();
}
```

## Debugging Advanced Actors

### Fast Actor Latency Analysis

```cpp
class instrumented_fast_actor : public cas::fast_actor {
private:
    std::chrono::microseconds m_total_latency{0};
    std::chrono::microseconds m_max_latency{0};
    int m_message_count = 0;

protected:
    void on_tick(const tick& msg) {
        auto now = std::chrono::steady_clock::now();
        auto latency = std::chrono::duration_cast<std::chrono::microseconds>(
            now - msg.timestamp);

        m_message_count++;
        m_total_latency += latency;

        if (latency > m_max_latency) {
            m_max_latency = latency;
            std::cout << "New max latency: " << latency.count() << "μs" << std::endl;
        }
    }

public:
    void print_stats() const {
        std::cout << "Messages: " << m_message_count << std::endl;
        std::cout << "Avg latency: " << (m_total_latency.count() / m_message_count) << "μs" << std::endl;
        std::cout << "Max latency: " << m_max_latency.count() << "μs" << std::endl;
    }
};
```

### Inline Actor Thread Safety Validation

```cpp
// Detect multiple callers on non-thread-safe inline actor
class debug_inline_actor : public cas::inline_actor<false> {
private:
    std::thread::id m_caller_thread;
    bool m_first_call = true;

public:
    void method() {
        auto current_thread = std::this_thread::get_id();

        if (m_first_call) {
            m_caller_thread = current_thread;
            m_first_call = false;
        } else {
            // Verify same thread
            assert(m_caller_thread == current_thread &&
                   "Non-thread-safe inline actor called from multiple threads!");
        }

        // ... actual logic
    }
};
```

### Stateful Actor State Tracing

```cpp
class traced_stateful_actor : public cas::stateful_actor {
protected:
    void enter_state_a() {
        std::cout << "[" << name() << "] Entering state A" << std::endl;
        accept_message_type<msg_a>();
        reject_message_type<msg_b>();
    }

    void enter_state_b() {
        std::cout << "[" << name() << "] Entering state B" << std::endl;
        reject_message_type<msg_a>();
        accept_message_type<msg_b>();
    }

    void on_msg_a(const msg_a& msg) {
        std::cout << "[" << name() << "] Processing msg_a in state A" << std::endl;
        // ...
    }
};
```

## Next Steps

- [Best Practices](120_best_practices.md) - Design patterns and anti-patterns
- [Timers](90_timers.md) - Schedule messages with timers
- [Ask Pattern](80_ask_pattern.md) - Synchronous RPC-style calls
- [Dynamic Removal](70_dynamic_removal.md) - Stop actors at runtime
