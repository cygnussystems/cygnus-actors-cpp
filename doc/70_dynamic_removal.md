# Dynamic Actor Removal

Starting with version 0.2.0, Cygnus supports stopping individual actors at runtime while the system continues running. This enables dynamic actor lifecycle management for use cases like:

- Dynamically adding/removing market actors in trading systems
- Managing worker pools with varying demand
- Cleaning up idle connections or resources
- Implementing actor supervision patterns

## Quick Example

```cpp
// Create actors
auto worker1 = cas::system::create<worker_actor>();
auto worker2 = cas::system::create<worker_actor>();
auto worker3 = cas::system::create<worker_actor>();

cas::system::start();

// ... workers process messages ...

// Stop worker2 while system continues running
bool stopped = cas::system::stop_actor(worker2);
if (stopped) {
    std::cout << "Worker 2 stopped successfully" << std::endl;
}

// worker1 and worker3 continue running
```

## Core API

### Stop Actor by Reference

```cpp
bool cas::system::stop_actor(actor_ref ref, const stop_config& config = {});
```

Stops a single actor gracefully. Returns `true` if successful, `false` if actor not found or already stopped.

**Example:**
```cpp
auto actor = cas::system::create<my_actor>();
cas::system::start();

// Stop with default configuration
if (cas::system::stop_actor(actor)) {
    std::cout << "Actor stopped" << std::endl;
}
```

### Stop Actor by Name

```cpp
bool cas::system::stop_actor(const std::string& name, const stop_config& config = {});
```

Convenience method to stop an actor by registry name.

**Example:**
```cpp
auto actor = cas::system::create<my_actor>();
cas::system::start();

// Stop by name
if (cas::system::stop_actor("my_actor")) {
    std::cout << "Named actor stopped" << std::endl;
}
```

### Check if Actor is Running

```cpp
// System-level check
bool cas::system::is_actor_running(actor_ref ref);

// actor_ref method
bool actor_ref.is_running() const;
```

Check if an actor is currently running.

**Example:**
```cpp
if (actor.is_running()) {
    actor.tell(message{});
} else {
    std::cout << "Actor is not running" << std::endl;
}
```

## Stop Configuration

Configure how actors are stopped using `cas::stop_config`:

```cpp
struct stop_config {
    stop_mode mode = stop_mode::drain;        // drain or discard
    std::chrono::milliseconds drain_timeout{1000};  // Max wait time
    bool wait_for_stop{true};                 // Synchronous vs async
    bool notify_watchers{true};               // Send termination_msg
};
```

### Stop Modes

#### Drain Mode (Default)

Process all pending messages before stopping:

```cpp
cas::stop_config config;
config.mode = cas::stop_mode::drain;
config.drain_timeout = std::chrono::milliseconds(2000);

cas::system::stop_actor(actor, config);
```

**Behavior:**
- All queued messages are processed
- Waits up to `drain_timeout` for queue to empty
- Actor's `on_shutdown()` and `on_stop()` called after draining
- Safe for actors with important pending work

#### Discard Mode

Drop pending messages immediately:

```cpp
cas::stop_config config;
config.mode = cas::stop_mode::discard;

cas::system::stop_actor(actor, config);
```

**Behavior:**
- Pending messages are discarded
- Lifecycle hooks called immediately
- Faster shutdown
- Use when pending messages can be safely ignored

### Synchronous vs Asynchronous Stop

#### Synchronous (Default)

Block until actor is fully stopped:

```cpp
cas::stop_config config;
config.wait_for_stop = true;  // Default

auto start = std::chrono::steady_clock::now();
bool stopped = cas::system::stop_actor(actor, config);
auto duration = std::chrono::steady_clock::now() - start;

// Function blocks until actor fully stopped
std::cout << "Stop took " << duration.count() << "ms" << std::endl;
```

**Use when:**
- You need to know actor is stopped before continuing
- Sequential cleanup is required
- Testing or debugging

#### Asynchronous

Initiate stop and return immediately:

```cpp
cas::stop_config config;
config.wait_for_stop = false;

bool initiated = cas::system::stop_actor(actor, config);
// Returns immediately, actor stops in background

// Check later if stopped
std::this_thread::sleep_for(std::chrono::milliseconds(100));
if (!actor.is_running()) {
    std::cout << "Actor has stopped" << std::endl;
}
```

**Use when:**
- You don't need to block
- Stopping multiple actors concurrently
- Performance-critical code paths

## Lifecycle During Removal

When `stop_actor()` is called, the following sequence occurs:

1. **State Transition** - Actor state changed from `running` to `stopping` (atomic)
2. **Timer Cancellation** - All active timers for this actor are cancelled
3. **Message Draining** - If drain mode, wait for queue to empty (up to timeout)
4. **on_shutdown() Hook** - Actor can send final messages, save state
5. **on_stop() Hook** - Final cleanup, no message sending allowed
6. **Watcher Notification** - Watchers tell `termination_msg` (if enabled)
7. **Thread Removal** - Actor removed from worker thread or dedicated thread joined
8. **Registry Cleanup** - Actor unregistered from registry (if named)
9. **State Final** - Actor state set to `stopped`

**Example with lifecycle hooks:**

```cpp
class monitored_actor : public cas::actor {
protected:
    void on_start() override {
        set_name("monitored");
        handler<work>(&monitored_actor::on_work);
    }

    void on_shutdown() override {
        std::cout << "Actor shutting down..." << std::endl;

        // Can still send messages here
        auto logger = cas::actor_registry::get("logger");
        if (logger.is_valid()) {
            log_msg msg;
            msg.text = "monitored_actor shutting down";
            logger.tell(msg);
        }

        // Save state
        save_state_to_database();
    }

    void on_stop() override {
        std::cout << "Actor stopped (final cleanup)" << std::endl;
        // No message sending allowed here
        // Final resource cleanup only
    }

    void on_work(const work& msg) {
        // Normal message processing
    }
};
```

## Watch Pattern

Monitor other actors for termination using the watch pattern:

### Watch an Actor

```cpp
void cas::system::watch(actor_ref watcher, actor_ref watched);
```

Register `watcher` to tell notification when `watched` stops.

### Unwatch an Actor

```cpp
void cas::system::unwatch(actor_ref watcher, actor_ref watched);
```

Remove watch relationship.

### Termination Message

When a watched actor stops, watchers tell `cas::termination_msg`:

```cpp
struct termination_msg : public cas::message_base {
    std::string actor_name;
    size_t instance_id;
    std::string type_name;
};
```

### Complete Example

```cpp
class supervisor : public cas::actor {
private:
    cas::actor_ref m_worker;

protected:
    void on_start() override {
        set_name("supervisor");
        handler<termination_msg>(&supervisor::on_worker_terminated);

        // Create and watch worker
        m_worker = cas::system::create<worker_actor>();
        cas::system::watch(self(), m_worker);
    }

    void on_worker_terminated(const termination_msg& msg) {
        std::cout << "Worker terminated: " << msg.actor_name
                  << " (instance " << msg.instance_id << ")" << std::endl;

        // Restart worker
        m_worker = cas::system::create<worker_actor>();
        cas::system::watch(self(), m_worker);
    }

    void stop_worker() {
        cas::system::stop_actor(m_worker);
        // Will tell termination_msg when worker stops
    }
};
```

## Behavior and Edge Cases

### Messages Sent to Stopped Actors

Messages sent to stopped actors are **silently dropped**:

```cpp
cas::system::stop_actor(actor);

// This message will be dropped
actor.tell(message{});  // No-op, returns immediately
```

### Concurrent Stop Attempts

Only the first call succeeds:

```cpp
// Thread 1
bool result1 = cas::system::stop_actor(actor);  // true

// Thread 2 (concurrent)
bool result2 = cas::system::stop_actor(actor);  // false (already stopping)
```

### Stop Non-Existent Actor

Returns `false`:

```cpp
cas::actor_ref invalid_ref;
bool result = cas::system::stop_actor(invalid_ref);  // false

bool result2 = cas::system::stop_actor("nonexistent");  // false
```

### Remaining Actors Continue

Stopping one actor doesn't affect others:

```cpp
auto actor1 = cas::system::create<my_actor>();
auto actor2 = cas::system::create<my_actor>();
auto actor3 = cas::system::create<my_actor>();

cas::system::start();

// Stop actor2
cas::system::stop_actor(actor2);

// actor1 and actor3 continue processing messages normally
actor1.tell(msg);  // Works
actor3.tell(msg);  // Works
actor2.tell(msg);  // Dropped (actor stopped)
```

### Fast Actors

Fast actors are also supported:

```cpp
class low_latency_worker : public cas::fast_actor {
    // ...
};

auto fast = cas::system::create<low_latency_worker>();
cas::system::start();

// Stop fast actor (dedicated thread will be joined)
cas::system::stop_actor(fast);
```

## Use Case: Dynamic Worker Pool

```cpp
class worker_pool_manager : public cas::actor {
private:
    std::vector<cas::actor_ref> m_workers;
    std::atomic<size_t> m_next_worker{0};

protected:
    void on_start() override {
        set_name("pool_manager");
        handler<add_worker>(&worker_pool_manager::on_add_worker);
        handler<remove_worker>(&worker_pool_manager::on_remove_worker);
        handler<distribute_work>(&worker_pool_manager::on_distribute_work);

        // Start with 3 workers
        resize_pool(3);
    }

    void resize_pool(size_t target_size) {
        while (m_workers.size() < target_size) {
            add_worker_impl();
        }

        while (m_workers.size() > target_size) {
            remove_worker_impl();
        }
    }

    void add_worker_impl() {
        auto worker = cas::system::create<worker_actor>();
        m_workers.push_back(worker);
        std::cout << "Worker added, pool size: " << m_workers.size() << std::endl;
    }

    void remove_worker_impl() {
        if (!m_workers.empty()) {
            auto worker = m_workers.back();
            m_workers.pop_back();

            // Stop worker gracefully
            cas::stop_config config;
            config.mode = cas::stop_mode::drain;  // Finish pending work
            config.drain_timeout = std::chrono::seconds(5);

            cas::system::stop_actor(worker, config);
            std::cout << "Worker removed, pool size: " << m_workers.size() << std::endl;
        }
    }

    void on_add_worker(const add_worker& msg) {
        add_worker_impl();
    }

    void on_remove_worker(const remove_worker& msg) {
        remove_worker_impl();
    }

    void on_distribute_work(const distribute_work& msg) {
        if (!m_workers.empty()) {
            // Round-robin distribution
            size_t idx = m_next_worker.fetch_add(1) % m_workers.size();
            m_workers[idx].tell(msg.task);
        }
    }
};
```

## Best Practices

### DO: Use Drain Mode for Important Work

```cpp
cas::stop_config config;
config.mode = cas::stop_mode::drain;
config.drain_timeout = std::chrono::seconds(10);  // Generous timeout

cas::system::stop_actor(actor, config);
```

### DO: Clean Up State in on_shutdown()

```cpp
void on_shutdown() override {
    // Save state
    database.save(m_state);

    // Notify dependencies
    notify_peers_of_shutdown();

    // Close connections
    m_connection.close();
}
```

### DO: Check is_running() Before Sending

```cpp
if (worker.is_running()) {
    worker.tell(task{});
} else {
    // Handle stopped actor
    reassign_task(task);
}
```

### DON'T: Send Messages in on_stop()

```cpp
void on_stop() override {
    // ✗ BAD: Don't send messages here
    // logger.tell(msg);  // Will be dropped!

    // ✓ GOOD: Just cleanup
    close_file_handles();
    free_resources();
}
```

### DON'T: Hold actor_ref Past Shutdown

```cpp
class bad_example {
    cas::actor_ref m_cached_actor;  // ✗ BAD: Can become invalid

public:
    void use_actor() {
        // Actor may have been stopped
        if (m_cached_actor.is_running()) {  // Check first!
            m_cached_actor.tell(msg);
        }
    }
};
```

## Thread Safety

All removal operations are thread-safe:

```cpp
// Multiple threads can safely call stop_actor concurrently
std::thread t1([&]() { cas::system::stop_actor(actor1); });
std::thread t2([&]() { cas::system::stop_actor(actor2); });
std::thread t3([&]() { cas::system::stop_actor(actor3); });

t1.join();
t2.join();
t3.join();
```

## Performance Considerations

- **Synchronous stop**: Blocks caller until complete (may take up to drain_timeout)
- **Asynchronous stop**: Returns immediately, minimal overhead
- **Drain mode**: Processes all pending messages (can take time)
- **Discard mode**: Fast, drops messages immediately
- **Watch notifications**: Small overhead per watcher (copy of watcher set)

## Comparison with System Shutdown

| Feature | stop_actor() | system::shutdown() |
|---------|-------------|-------------------|
| Scope | Single actor | All actors |
| System continues | Yes | No |
| Other actors | Unaffected | All stop |
| Configuration | Per-actor | Global |
| Typical use | Runtime management | Application exit |

## Next Steps

- [Best Practices](120_best_practices.md) - Design patterns and guidelines
- [Advanced Actors](100_advanced_actors.md) - Fast, inline, and stateful actors
- [Configuration](110_configuration.md) - System configuration options
