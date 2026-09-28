# Dynamic Actor Removal

Starting with version 0.2.0, Cygnus supports stopping individual actors at runtime while the system continues running. This enables dynamic actor lifecycle management for use cases like:

- Dynamically adding/removing market actors in trading systems
- Managing worker pools with varying demand
- Cleaning up idle connections or resources
- Implementing actor supervision patterns

## Quick Example

> **Runnable versions:** `tutorials/08_dynamic_removal.cpp` (`tutorials 8`)
> for stopping an actor, drain vs discard, and stale references;
> `tutorials/09_watch_pattern.cpp` (`tutorials 9`) for the watch pattern.
> Both are compiled and executed by `ctest`.

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
- The actor stops accepting new messages immediately (they become dead letters)
- Messages already queued keep being processed, on the actor's own thread,
  until the queue is empty or `drain_timeout` (measured from the call) expires
- Then `on_shutdown()` and `on_stop()` run, on the actor's own thread
- Safe for actors with important pending work

#### Discard Mode

Drop pending messages immediately:

```cpp
cas::stop_config config;
config.mode = cas::stop_mode::discard;

cas::system::stop_actor(actor, config);
```

**Behavior:**
- Queued messages are dropped without being handled, and counted as dead
  letters (pending `call()` callers get `std::future_error`, broken promise)
- A handler already running finishes first; then the hooks run
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
// Returns once the stop is requested; is_running() is already false.
// Draining/discarding and on_shutdown()/on_stop() happen on the actor's thread.

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

#### When `stop_actor()` does not block

Waiting would deadlock if the calling thread is the one that has to carry out
the stop, so in these cases the call returns without waiting (as if
`wait_for_stop = false`) and the stop completes as soon as the current handler
returns:

- an actor stopping itself from its own handler
- a handler stopping an actor that runs on the same worker thread
- a fast actor's own thread
- after `system::shutdown()` has been requested (the stop is then completed by
  `wait_for_shutdown()`)

Before `start()` or after `wait_for_shutdown()` no actor thread is running, and
`stop_actor()` performs the whole stop on the calling thread.

## Lifecycle During Removal

When `stop_actor()` is called, the calling thread only does step 1; everything
else runs on the thread that executes the actor (its pool worker, or a fast
actor's dedicated thread), serialised with its handlers. No handler runs
concurrently with, or after, `on_stop()`.

1. **State Transition** - `running` to `stopping` (atomic); new messages are dead letters
2. **Timer Cancellation** - All active timers for this actor are cancelled
3. **Discard or Drain** - discard: drop the queue; drain: process it until empty or `drain_timeout`
4. **on_shutdown() Hook** - Actor can send final messages, save state
5. **on_stop() Hook** - Final cleanup
6. **Registry Cleanup** - Name unregistered, unless a replacement has already taken it
7. **Watcher Notification** - Watchers receive `termination_msg` (if enabled)
8. **Thread Removal** - Removed from its worker; a fast actor's thread is joined
9. **State Final** - Actor state set to `stopped`; a blocking `stop_actor()` returns

The registry is cleaned up before watchers are notified, so a supervisor that
restarts the actor under the same name from its `termination_msg` handler
keeps its new registration.

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

Register `watcher` to receive a `termination_msg` when `watched` is stopped with
`stop_actor()`. Watching the same actor twice is one watch.

### Unwatch an Actor

```cpp
void cas::system::unwatch(actor_ref watcher, actor_ref watched);
```

Remove watch relationship.

### What is guaranteed

- Each live watcher receives **exactly one** `termination_msg` per watched actor
  that is stopped with `stop_actor()` (unless `stop_config::notify_watchers` is
  `false`).
- **Watches do not keep actors alive.** A watcher destroyed before the actor it
  watches is skipped; a watcher that has already stopped is not sent anything.
- **A stopping actor's own watches end too**: it is removed from everything it
  was watching, so short-lived watchers of a long-lived actor do not pile up.
- Watching an actor that has already stopped registers nothing and sends
  nothing - check `is_running()` first if you need to know.
- Whole-system shutdown (`system::shutdown()`) does not send termination
  messages, and `system::reset()` forgets all watches.

### Termination Message

When a watched actor stops, each watcher receives `cas::termination_msg`:

```cpp
struct termination_msg : public cas::message_base {
    std::string actor_name;
    size_t instance_id;
    std::string type_name;
};
```

### Complete Example

> **Creating actors at runtime:** `system::create()` calls the new actor's
> `on_start()` before returning whenever the system is already running, so the
> actor is ready to receive messages immediately. When the system has not started
> yet, `start()` calls it instead — either way `on_start()` runs exactly once.
> Calling `create()` from inside a message handler is safe.

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
        // on_worker_terminated() is called once the worker has stopped
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
- System configuration - `system_config`, `shutdown_config` and `stop_config`
  are declared with comments in `include/cas/system.h`
