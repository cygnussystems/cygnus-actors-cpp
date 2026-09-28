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

Stops a single actor gracefully. Returns `true` if this call started the stop.
When it returns without waiting (see
[When `stop_actor()` does not block](#when-stop_actor-does-not-block)), the stop
may still be in progress. Returns `false` if the ref is invalid, or the actor is
already stopping or stopped - including by system shutdown or a concurrent
`stop_actor()`.

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
    std::chrono::milliseconds drain_timeout{1000};  // Drain budget, measured from the
                                                    // stop_actor() call; does not bound a
                                                    // running handler or the hooks
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
std::cout << "Stop took "
          << std::chrono::duration_cast<std::chrono::milliseconds>(duration).count()
          << "ms" << std::endl;
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
`stop_actor()` performs the whole stop on the calling thread. An actor stopped
before `start()` never runs `on_start()`, but its `on_shutdown()` and
`on_stop()` do run.

Inside a handler, prefer `wait_for_stop = false` and learn about completion
from a `termination_msg` (see [Watch Pattern](#watch-pattern)). A synchronous
stop of an actor on another worker blocks this worker thread - and every actor
scheduled on it - for up to `drain_timeout` plus the target's hooks, and two
actors on different workers that synchronously stop each other deadlock.

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
6. **Close** - Timers scheduled by the hooks are cancelled; the mailbox is closed,
   and anything still queued (e.g. after a drain timeout) is a dead letter
7. **Registry Cleanup** - Name unregistered, unless a replacement has already taken it
8. **Watcher Notification** - Watchers receive `termination_msg` (if enabled)
9. **Thread Removal** - Removed from its worker
10. **State Final** - Actor state set to `stopped`; a blocking `stop_actor()` returns.
    A fast actor's thread is joined after this - by `stop_actor()` if it waited,
    otherwise later (e.g. by `system::actor_count()`)

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
        // Final resource cleanup only. Messages sent from here are delivered
        // after stop_actor(), but are dead letters during system shutdown.
    }

    void on_work(const work& msg) {
        // Normal message processing
    }
};
```

## Creating and Removing Actors at Runtime

A running system can grow and shrink: for example an actor per trading system,
created when the trading system is launched and stopped when it completes, and
a streaming actor per subscriber, created and removed as subscribers come and
go.

### Creating

- `system::create<T>(args...)` is safe from any thread - a handler, `on_start()`,
  `main()`, or a non-actor thread such as a network acceptor.
- While the system is running, the new actor's `on_start()` runs on the creating
  thread, **before `create()` returns** - for pooled and fast actors alike.
  Messages sent to it meanwhile are queued, and none is handled until
  `on_start()` returns. `call()` from `on_start()` throws `on_start_violation`.
- Pooled actors are assigned to worker threads round-robin; there is no
  affinity control.
- **Use pooled actors for per-subscriber or per-instance actors.** Each
  `fast_actor` owns an OS thread that polls (yield, hybrid or busy-wait), so a
  fast actor per subscriber costs a thread - and potentially a core - each.
  Reserve `fast_actor` for a few latency-critical actors such as an exchange
  session.
- Give dynamic actors unique names (e.g. `"stream." + client_id`) or none.
  Default names are not registered. Re-using a name after a restart is safe:
  a stopping actor only unregisters its name if the name still refers to it.

### Removing

- An actor that knows it is done stops itself: `cas::system::stop_actor(self())`
  from a handler returns at once, and the stop completes after the handler
  returns.
- Whoever needs to react - a manager, a supervisor - `watch()`es it and gets a
  `termination_msg` (match on `instance_id`: names can be reused).
- From a handler, stop other actors with `wait_for_stop = false` (see
  [When `stop_actor()` does not block](#when-stop_actor-does-not-block)).
- Messages still arriving for a removed actor are dead letters (counted, and
  passed to the dead-letter handler) - expect some around every removal. The
  dead-letter and queue-threshold handlers (`system::set_dead_letter_handler()`,
  `system::set_queue_threshold_handler()`) are the operational signal for
  traffic to removed actors and for subscribers that fall behind.
- `stateful_actor` / `stateful_fast_actor`: held (stashed) messages are reported
  as dead letters when the actor stops. An `inline_actor` rejects messages once
  stopping, and its stop is serialised with its synchronous dispatch.

### Memory

After `stop_actor()` the system drops its own references: pooled actors
immediately, fast actors once their thread has been joined (by `stop_actor()`
or `system::actor_count()`). The registry and the watch table hold only weak
references. The actor object is freed when the **last `actor_ref`** to it goes -
including refs in other actors' members and in `message_base::sender` of
messages still held somewhere. Two actors holding `actor_ref` members to each
other form a `shared_ptr` cycle and are never freed: clear such members in
`on_stop()`. `system::actor_count()` counts live actors; instance ids are never
reused until `system::reset()`.

### Interaction with System Shutdown

- A stop still in progress when `shutdown()` is called is completed by
  `wait_for_shutdown()`.
- `stop_actor()` returns `false` for an actor that has already handled its
  system shutdown message.
- System shutdown sends no `termination_msg`, so a supervisor does not restart
  actors during shutdown.

### Example: Trading Systems and Their Streams

```cpp
struct launch_trading_system : public cas::message_base {
    std::string system_id;
    std::vector<std::string> subscribers;
};
struct market_update : public cas::message_base { double price = 0.0; };
struct run_complete : public cas::message_base {};

// One per subscriber: pooled, unnamed
class stream_actor : public cas::actor {
    std::string m_subscriber;
public:
    explicit stream_actor(std::string subscriber) : m_subscriber(std::move(subscriber)) {}
protected:
    void on_start() override {
        handler<market_update>(&stream_actor::on_update);
    }
    void on_update(const market_update& msg) {
        // push msg.price to m_subscriber's connection ...
    }
};

class trading_system_actor : public cas::actor {
    std::string m_id;
    std::vector<cas::actor_ref> m_streams;
public:
    trading_system_actor(std::string id, std::vector<cas::actor_ref> streams)
        : m_id(std::move(id)), m_streams(std::move(streams)) {}
protected:
    void on_start() override {
        set_name("ts." + m_id);
        handler<market_update>(&trading_system_actor::on_update);
        handler<run_complete>(&trading_system_actor::on_complete);
    }
    void on_update(const market_update& msg) {
        for (const auto& stream : m_streams) stream.tell(msg);
    }
    void on_complete(const run_complete&) {
        cas::system::stop_actor(self());  // returns at once; stops after this handler
    }
    void on_stop() override {
        m_streams.clear();  // release the refs
    }
};

class trading_manager : public cas::actor {
    // trading system instance id -> its streaming actors
    std::unordered_map<size_t, std::vector<cas::actor_ref>> m_streams;
protected:
    void on_start() override {
        set_name("trading_manager");
        handler<launch_trading_system>(&trading_manager::on_launch);
        handler<cas::termination_msg>(&trading_manager::on_terminated);
    }
    void on_launch(const launch_trading_system& msg) {
        std::vector<cas::actor_ref> streams;
        for (const auto& subscriber : msg.subscribers) {
            streams.push_back(cas::system::create<stream_actor>(subscriber));
        }
        // Each create() has run the new actor's on_start() before returning
        auto ts = cas::system::create<trading_system_actor>(msg.system_id, streams);
        cas::system::watch(self(), ts);
        m_streams[ts.get_actor()->instance_id()] = std::move(streams);
    }
    void on_terminated(const cas::termination_msg& msg) {
        auto it = m_streams.find(msg.instance_id);
        if (it == m_streams.end()) return;

        cas::stop_config config;
        config.wait_for_stop = false;  // never block this worker thread
        for (const auto& stream : it->second) {
            cas::system::stop_actor(stream, config);
        }
        m_streams.erase(it);  // drop our refs so the actors can be freed
    }
};

// main():
//   auto manager = cas::system::create<trading_manager>();
//   cas::system::start();
//   launch_trading_system launch;
//   launch.system_id = "es_momentum";
//   launch.subscribers = {"client_a", "client_b"};
//   manager.tell(launch);
//   ...
//   cas::actor_registry::get("ts.es_momentum").tell(run_complete{});
```

A subscriber that leaves while its trading system keeps running is handled the
same way: stop its `stream_actor` with `wait_for_stop = false` and tell the
trading system to drop the ref; updates sent to it in between are dead letters.

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

```cpp
class supervisor : public cas::actor {
private:
    cas::actor_ref m_worker;

protected:
    void on_start() override {
        set_name("supervisor");
        handler<cas::termination_msg>(&supervisor::on_worker_terminated);

        // Create and watch worker
        m_worker = cas::system::create<worker_actor>();
        cas::system::watch(self(), m_worker);
    }

    void on_worker_terminated(const cas::termination_msg& msg) {
        std::cout << "Worker terminated: " << msg.actor_name
                  << " (instance " << msg.instance_id << ")" << std::endl;

        // Restart worker
        m_worker = cas::system::create<worker_actor>();
        cas::system::watch(self(), m_worker);
    }

    void stop_worker() {
        cas::stop_config config;
        config.wait_for_stop = false;  // don't block this worker thread
        cas::system::stop_actor(m_worker, config);
        // on_worker_terminated() is called once the worker has stopped
    }
};
```

## Behavior and Edge Cases

### Messages Sent to Stopped Actors

Messages sent to a stopping or stopped actor are dropped and reported as dead
letters: counted in `system::get_dead_letter_stats()` and passed to the handler
set with `system::set_dead_letter_handler()`. A `call()` to it throws
`std::future_error` (broken promise), with or without a timeout.

```cpp
cas::system::stop_actor(actor);

// Dropped: dead_letter_stats::dropped_tell is incremented
actor.tell(message{});
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
            config.wait_for_stop = false;         // Don't block this worker thread

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

cas::system::stop_actor(actor, config);  // from main(); in a handler also set
                                          // wait_for_stop = false
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
task t;
if (worker.is_running()) {
    worker.tell(t);
} else {
    // Handle stopped actor
    reassign_task(t);
}
```

The check is racy: the actor can stop between `is_running()` and `tell()`, so
a dead-letter handler is still the place to catch lost work.

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

### DON'T: Keep References to Stopped Actors

An `actor_ref` owns the actor (it holds a `shared_ptr`). It stays valid after
the actor stops - `is_running()` is then `false` - and keeps the stopped
actor's memory alive. Drop it once the actor has stopped (for example on
`termination_msg`).

```cpp
class bad_example {
    cas::actor_ref m_cached_actor;  // ✗ Stays valid but stopped; keeps the object alive

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
| Hooks run on | Actor's own thread | `on_shutdown()` on own thread; pooled `on_stop()` on the `wait_for_shutdown()` caller |
| Queued messages | Drained or discarded per call | Drained until the global `drain_timeout`; leftovers only logged |
| Name unregistered | Yes | No |
| Watchers notified | Yes (unless `notify_watchers = false`) | No |
| Removed from system | Yes | No (until `system::reset()`) |
| Typical use | Runtime management | Application exit |

## Next Steps

- [Best Practices](120_best_practices.md) - Design patterns and guidelines
- [Advanced Actors](100_advanced_actors.md) - Fast, inline, and stateful actors
- System configuration - `system_config`, `shutdown_config` and `stop_config`
  are declared with comments in `include/cas/system.h`
