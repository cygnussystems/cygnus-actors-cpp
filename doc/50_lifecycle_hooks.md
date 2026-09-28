# Actor Lifecycle Hooks

Actors have a well-defined lifecycle with three hooks that give you precise control over initialization, shutdown preparation, and final cleanup.

> **Runnable version:** `tutorials/04_lifecycle_hooks.cpp` — run `tutorials 4`.
> It is compiled and executed by `ctest`, and shows the hook order and message
> draining as observable output.

## Lifecycle Overview

```
┌──────────────┐
│   Created    │  system::create<T>()
└──────┬───────┘
       │
       ▼
┌──────────────┐
│  on_start()  │  System starts, actor initializes
└──────┬───────┘
       │
       ▼
┌──────────────┐
│   Running    │  Processing messages
│              │  (most of actor's lifetime)
└──────┬───────┘
       │
       ▼
┌──────────────┐
│ on_shutdown()│  Shutdown initiated, can still send messages (see caveat below)
└──────┬───────┘
       │
       ▼
┌──────────────┐
│  Draining    │  Processing remaining messages
└──────┬───────┘
       │
       ▼
┌──────────────┐
│  on_stop()   │  Final cleanup, no message sending
└──────┬───────┘
       │
       ▼
┌──────────────┐
│   Stopped    │  Actor terminated
└──────────────┘
```

This is the order for whole-system shutdown. `stop_actor()` drains (or
discards) first and then runs `on_shutdown()` and `on_stop()` together - see
[Call Order Guarantees](#call-order-guarantees).

## The Three Hooks

### on_start()

Called once per actor, before it handles any message.

- Pooled actors created **before** `system::start()` are started by `start()`,
  on the thread that calls it, before any worker thread exists. A `fast_actor`
  created before `start()` runs `on_start()` on its own dedicated thread, which
  may be after `start()` has returned.
- Actors created **after** the system is running - pooled or fast - are started
  inside `create()`, on the creating thread, before it returns. Messages sent to
  the new actor meanwhile are queued; none is handled until `on_start()` returns.

Either way `on_start()` runs exactly once. See
[Creating and removing actors at runtime](70_dynamic_removal.md#creating-and-removing-actors-at-runtime).

> **Warning - `on_start()` must not throw.** A failed `on_start()` is treated
> as a programming error, not something to recover from:
> - inside `create()` (system running): the exception propagates out of
>   `create()` and the actor is discarded;
> - inside `start()`: it propagates out of `start()` and the system is left
>   half-started - shut down and exit;
> - for a `fast_actor` created before `start()`, `on_start()` runs on the
>   actor's own thread: the exception is reported (see
>   `system::set_handler_exception_handler()`) and then **the process is
>   terminated**.
>
> Do fallible set-up (connections, file loads) in a handler for a message the
> actor sends itself from `on_start()`, where an exception is caught and
> reported like any other handler's.

**Signature:**
```cpp
protected:
    void on_start() override;
```

**Purpose:** `on_start()` is for initialising *this* actor.

- Initialize actor state
- Register message handlers
- Lookup other actors in registry
- Set actor name
- Start timers
- Send initial messages with `tell()`

**Permitted:**

| Operation | Notes |
|---|---|
| `set_name()` | Also registers in the actor registry |
| `handler<T>()`, `call_handler<>()` | The main purpose of the hook |
| Member initialisation | |
| `schedule_once()`, `schedule_periodic()` | Timers fire once the system is running |
| `tell()` / `operator<<` | Queued; delivered once workers start |
| `system::create()` | Safe on both the startup and runtime paths |
| `actor_registry::get()` | See the ordering caveat below |

**Not permitted:**

| Operation | Why | Result |
|---|---|---|
| `call()` | Blocks waiting for a reply from a system that may not be processing messages yet — at startup no worker thread exists | Throws `cas::on_start_violation` |

```cpp
void on_start() override {
    set_name("my_actor");
    handler<work_msg>(&my_actor::on_work);

    auto other = cas::actor_registry::get("other_actor");

    other.tell(hello_msg{});           // ✓ fine - queued for delivery
    // int r = other.call<int>(op{});   // ✗ throws cas::on_start_violation
}
```

If you need a value from another actor during startup, `tell()` a request and
handle the reply in a normal message handler. That keeps initialisation
non-blocking, which is what lets the whole system come up.

> **Registry ordering:** `actor_registry::get()` only finds actors whose
> `on_start()` has already run, since `set_name()` is what registers them. The
> order in which actors' `on_start()` runs is unspecified (it is not creation
> order, and fast actors start on their own threads), so a lookup of any other
> actor from `on_start()` may fail. Prefer passing an `actor_ref` through the
> constructor when the dependency is known, or do the lookup lazily in a handler.

**Example:**
```cpp
class worker : public cas::actor {
private:
    cas::actor_ref m_manager;
    int m_task_count = 0;

protected:
    void on_start() override {
        // Set name for registry lookup
        set_name("worker_1");

        // Register message handlers
        handler<task>(&worker::on_task);
        handler<status_request>(&worker::on_status_request);

        // Lookup other actors
        m_manager = cas::actor_registry::get("manager");

        // Send initial message
        if (m_manager.is_valid()) {
            ready_msg msg;
            m_manager.tell(msg);
        }
    }

    void on_task(const task& msg) {
        m_task_count++;
        // Process task...
    }

    void on_status_request(const status_request& msg) {
        // Reply with status
    }
};
```

**What You CAN Do:**
- ✓ Register handlers
- ✓ Set name
- ✓ Lookup actors in registry
- ✓ Initialize member variables
- ✓ Send messages
- ✓ Schedule timers
- ✓ Create child actors (if needed)

**What You CANNOT Do:**
- ✗ Rely on all actors being ready (some may not have started yet)
- ✗ Block for long periods (delays system startup)
- ✗ Throw exceptions (causes system startup to fail)

### on_shutdown()

Called when shutdown is initiated (either system-wide or individual actor stop).

**Signature:**
```cpp
protected:
    void on_shutdown() override;  // Optional - default does nothing
```

**Purpose:**
- Save state to database
- Send final notifications
- Close network connections
- Flush buffers
- Notify dependent actors
- Cancel subscriptions

**Example:**
```cpp
class database_actor : public cas::actor {
private:
    std::vector<std::string> m_pending_writes;
    cas::actor_ref m_logger;

protected:
    void on_start() override {
        set_name("database");
        handler<write_request>(&database_actor::on_write);
        m_logger = cas::actor_registry::get("logger");
    }

    void on_write(const write_request& msg) {
        m_pending_writes.push_back(msg.data);
    }

    void on_shutdown() override {
        // Flush pending writes
        if (!m_pending_writes.empty()) {
            flush_to_database(m_pending_writes);
            m_pending_writes.clear();
        }

        // Notify logger
        if (m_logger.is_valid()) {
            log_msg msg;
            msg.text = "Database actor shutting down";
            msg.severity = "INFO";
            m_logger.tell(msg);  // Can still send messages
        }

        std::cout << "Database actor: shutdown complete" << std::endl;
    }
};
```

**What You CAN Do:**
- ✓ Send messages (to notify other actors)
- ✓ Save state to disk/database
- ✓ Close connections
- ✓ Flush buffers
- ✓ Log shutdown events
- ✓ Unsubscribe from services

**What You CANNOT Do:**
- ✗ Rely on other actors still running (they may be shutting down too)
- ✗ Block indefinitely (system has timeout)
- ✗ Throw exceptions (may cause unclean shutdown)

### on_stop()

Called last. After `stop_actor()` it runs on the actor's own thread, right after
draining and `on_shutdown()`. In a system shutdown it is called from
`wait_for_shutdown()`: for pooled actors on the thread calling
`wait_for_shutdown()`, after the workers are joined; fast actors run it on their
own thread. The actor object is destroyed only when the last `actor_ref` to it
is released.

**Signature:**
```cpp
protected:
    void on_stop() override;  // Optional - default does nothing
```

**Purpose:**
- Final resource cleanup
- Close file handles
- Free memory
- Log final state
- Don't rely on sending messages: during system shutdown they become dead
  letters (see below)

**Example:**
```cpp
class file_processor : public cas::actor {
private:
    std::ofstream m_output_file;
    int m_lines_processed = 0;

protected:
    void on_start() override {
        set_name("processor");
        handler<process_line>(&file_processor::on_process_line);
        m_output_file.open("output.txt");
    }

    void on_process_line(const process_line& msg) {
        if (m_output_file.is_open()) {
            m_output_file << msg.data << "\n";
            m_lines_processed++;
        }
    }

    void on_shutdown() override {
        // Flush file before closing
        if (m_output_file.is_open()) {
            m_output_file.flush();
        }

        std::cout << "Processed " << m_lines_processed << " lines" << std::endl;
    }

    void on_stop() override {
        // Final cleanup - close file
        if (m_output_file.is_open()) {
            m_output_file.close();
        }

        // Log final state (to stdout, NOT to other actors)
        std::cout << "File processor stopped" << std::endl;

        // Don't rely on messages sent from here: during system shutdown every
        // actor is stopping, so they become dead letters.
    }
};
```

**What You CAN Do:**
- ✓ Close file handles
- ✓ Free resources
- ✓ Log to stdout/stderr
- ✓ Delete temporary files
- ✓ Final cleanup

**What You CANNOT Do:**
- ✗ Rely on messages sent from here being handled: during system shutdown
  every actor is already stopping, so they become (counted) dead letters.
  After `stop_actor()` the rest of the system is running and they are
  delivered - but send final messages from `on_shutdown()` instead.
- ✗ Expect other actors to be available
- ✗ Block (should complete quickly)

## Call Order Guarantees

### System-Wide Shutdown

When `system::shutdown()` is called:

1. **Every running actor is sent a shutdown message**, from the thread that
   called `shutdown()`.
2. **Each actor runs `on_shutdown()` when it reaches that message** in its
   mailbox - after the messages that thread sent it earlier - and then stops
   accepting new messages (they become dead letters) and its timers are
   cancelled. Actors do this
   independently; there is no order *between* actors. An `inline_actor`
   handles the shutdown message synchronously, so its `on_shutdown()` runs on
   the thread that called `shutdown()`. Actors already being stopped with
   `stop_actor()` are skipped; `wait_for_shutdown()` completes their stop.
3. **Draining:** messages already queued are still handled, until every
   mailbox is empty or `drain_timeout` expires.
4. **`wait_for_shutdown()`** joins the worker threads and runs `on_stop()` for
   every pooled actor on the calling thread (fast actors run theirs on their
   own thread).

System shutdown does not unregister names, does not send `termination_msg` to
watchers, and leaves the actors in the system until `system::reset()`.

```cpp
cas::system::shutdown();          // 1-3: shutdown sent, on_shutdown() runs, queues drain
cas::system::wait_for_shutdown(); // 4: threads joined, on_stop() called
```

**Caveat - messages from `on_shutdown()` to other actors:** such a message is
handled only if the receiving actor has not yet reached its own shutdown
message; otherwise it is a dead letter. Since actors shut down in no particular
order, do not rely on it. If teardown must happen in a particular order (for
example, stop the order router before the risk engine it reports to), stop the
actors in that order with `stop_actor()` before calling `shutdown()`.

### Individual Actor Stop

When `system::stop_actor()` is called:

1. **Actor state set to `stopping`** (on the calling thread; new messages are dead letters)
2. **Timers cancelled**; with `stop_mode::discard`, queued messages are dropped
   as dead letters
3. **Draining** (`stop_mode::drain`): queued messages are handled until the
   queue is empty or `drain_timeout` expires
4. **`on_shutdown()`, then `on_stop()` called**
5. **Timers scheduled by the hooks cancelled**; the mailbox is closed and
   anything still in it is a dead letter
6. **Name unregistered** (only if it still maps to this actor), **watchers
   notified**, actor removed from its worker, state set to `stopped`

Steps 2-6 run on the actor's own thread, never concurrently with one of its
handlers. See [Dynamic Removal](70_dynamic_removal.md#lifecycle-during-removal).

```cpp
cas::system::stop_actor(actor_ref);
// Blocks until on_stop() has returned if wait_for_stop=true - except when
// called from the actor itself, from its worker thread, from a fast actor's
// own thread, or after shutdown(); those complete asynchronously.
```

## Message Draining

### What is Message Draining?

When shutdown is initiated, actors may have pending messages in their queues. **Message draining** ensures these messages are processed before the actor stops.

```
Before shutdown:
┌──────────┐
│  Actor   │  Mailbox: [msg1, msg2, msg3, msg4]
└──────────┘

After shutdown signal:
┌──────────┐
│  Actor   │  State: stopping
└──────────┘  Processing: msg1 → msg2 → msg3 → msg4

After draining:
┌──────────┐
│  Actor   │  Mailbox: []  (empty)
└──────────┘  State: stopped
```

### Drain Behavior

**During draining:**
- Existing messages are processed
- New messages are **rejected**, and counted as dead letters
- Actor can send messages from `on_shutdown()` (see the caveat above)
- Actor processes messages normally
- Timeout prevents infinite waiting

**Example:**
```cpp
class counter : public cas::actor {
private:
    int m_count = 0;

protected:
    void on_start() override {
        handler<increment>(&counter::on_increment);
    }

    void on_increment(const increment& msg) {
        m_count++;
        std::cout << "Count: " << m_count << std::endl;
    }

    void on_shutdown() override {
        std::cout << "Final count: " << m_count << std::endl;
    }
};

int main() {
    auto counter_ref = cas::system::create<counter>();
    cas::system::start();

    // Send 10 messages
    for (int i = 0; i < 10; ++i) {
        counter_ref.tell(increment{});
    }

    // Shutdown immediately
    cas::system::shutdown();       // All 10 messages will be processed
    cas::system::wait_for_shutdown();

    // Output:
    // Count: 1
    // Count: 2
    // ...
    // Count: 10
    // Final count: 10
}
```

### Drain Timeout

The system waits up to `drain_timeout` for queues to empty:

```cpp
cas::shutdown_config config;
config.drain_timeout = std::chrono::milliseconds(5000);  // Wait up to 5 seconds

cas::system::shutdown(config);
```

If timeout is reached:
- Shutdown proceeds; messages still queued are **never handled**
- Each actor that still had messages is listed in
  `cas::system::get_shutdown_log()` ("Actor 'x' has N unprocessed messages at
  shutdown")
- They are not counted as dead letters

## Lifecycle Best Practices

### DO: Initialize in on_start()

```cpp
class my_actor : public cas::actor {
private:
    cas::actor_ref m_dependency;
    std::string m_config_value;

protected:
    void on_start() override {
        // ✓ GOOD: Initialize here
        set_name("my_actor");
        m_dependency = cas::actor_registry::get("other_actor");
        m_config_value = load_config();
        handler<msg>(&my_actor::on_msg);
    }
};

// ✗ BAD: Don't initialize in constructor
class bad_actor : public cas::actor {
private:
    cas::actor_ref m_dependency;

public:
    bad_actor() {
        // Too early: peers may not have registered yet, and self() is not
        // available until the system has taken the actor
        m_dependency = cas::actor_registry::get("other");
    }
};
```

### DO: Save State in on_shutdown()

```cpp
void on_shutdown() override {
    // ✓ GOOD: Save important state
    save_to_database(m_important_data);

    // Notify peers
    broadcast_shutdown_notification();
}
```

### DO: Release Resources in on_stop()

```cpp
void on_stop() override {
    // ✓ GOOD: Clean up resources
    if (m_file.is_open()) {
        m_file.close();
    }

    if (m_socket.is_open()) {
        m_socket.close();
    }
}
```

### DON'T: Send Messages in on_stop()

```cpp
void on_stop() override {
    // ✗ BAD: during system shutdown the logger is stopping too - this is a
    // dead letter. Send final messages from on_shutdown() instead.
    log_msg m;
    m.text = "Stopping";
    m_logger.tell(m);

    // ✓ GOOD: Use stdout for logging
    std::cout << "Actor stopping" << std::endl;
}
```

### DON'T: Block Indefinitely

```cpp
void on_shutdown() override {
    // ✗ BAD: Blocks shutdown
    while (!queue_is_empty()) {
        std::this_thread::sleep_for(1s);  // Could block forever
    }

    // ✓ GOOD: Respect timeout
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!queue_is_empty() && std::chrono::steady_clock::now() < deadline) {
        process_one_item();
    }
}
```

### DON'T: Throw Exceptions

```cpp
void on_start() override {
    // ✗ BAD: Throws exception
    if (!config_file_exists()) {
        throw std::runtime_error("Config not found");  // Crashes system
    }

    // ✓ GOOD: Handle errors gracefully
    if (!config_file_exists()) {
        std::cerr << "Warning: Config not found, using defaults" << std::endl;
        use_default_config();
    }
}
```

## Common Patterns

### Dependency Injection

```cpp
class service_actor : public cas::actor {
private:
    std::string m_service_name;
    int m_port;
    cas::actor_ref m_logger;

public:
    service_actor(const std::string& name, int port)
        : m_service_name(name), m_port(port) {
    }

protected:
    void on_start() override {
        set_name(m_service_name);
        handler<request>(&service_actor::on_request);

        // Lookup dependencies
        m_logger = cas::actor_registry::get("logger");

        // Log startup
        if (m_logger.is_valid()) {
            log_msg msg;
            msg.text = m_service_name + " starting on port " + std::to_string(m_port);
            m_logger.tell(msg);
        }
    }
};

// Usage
auto service = cas::system::create<service_actor>("api_server", 8080);
```

### Graceful Connection Cleanup

```cpp
class connection_actor : public cas::actor {
private:
    tcp_socket m_socket;
    std::string m_peer_address;

protected:
    void on_start() override {
        handler<data>(&connection_actor::on_data);
        m_socket.connect(m_peer_address);
    }

    void on_shutdown() override {
        // Send graceful disconnect
        if (m_socket.is_connected()) {
            m_socket.send("BYE\n");
            m_socket.flush();
        }
    }

    void on_stop() override {
        // Force close connection
        if (m_socket.is_open()) {
            m_socket.close();
        }
    }
};
```

### State Persistence

```cpp
class persistent_actor : public cas::actor {
private:
    std::map<std::string, int> m_state;
    std::string m_state_file;

protected:
    void on_start() override {
        m_state_file = "state_" + name() + ".dat";

        // Load persisted state
        load_state_from_file(m_state_file);

        handler<update>(&persistent_actor::on_update);
    }

    void on_update(const update& msg) {
        m_state[msg.key] = msg.value;
    }

    void on_shutdown() override {
        // Persist state before shutdown
        save_state_to_file(m_state_file);
        std::cout << "State saved (" << m_state.size() << " entries)" << std::endl;
    }

private:
    void load_state_from_file(const std::string& filename) {
        std::ifstream file(filename);
        if (file.is_open()) {
            // Deserialize state
            // ...
        }
    }

    void save_state_to_file(const std::string& filename) {
        std::ofstream file(filename);
        if (file.is_open()) {
            // Serialize state
            // ...
        }
    }
};
```

### Coordinated Shutdown

Teardown that must happen in order is done **before** `system::shutdown()`:
during system shutdown the workers may already be stopping (so `stop_actor()`
returns `false`), the coordinator stops accepting messages as soon as its own
`on_shutdown()` returns, and no `termination_msg` is sent.

```cpp
struct begin_teardown : public cas::message_base {};
struct teardown_complete : public cas::message_base {};

class coordinator : public cas::actor {
private:
    std::vector<cas::actor_ref> m_workers;
    cas::actor_ref m_requester;
    size_t m_workers_stopped = 0;

protected:
    void on_start() override {
        handler<begin_teardown>(&coordinator::on_begin_teardown);
        handler<cas::termination_msg>(&coordinator::on_worker_stopped);

        // Create workers and watch them
        for (int i = 0; i < 5; ++i) {
            auto worker = cas::system::create<worker_actor>();
            m_workers.push_back(worker);
            cas::system::watch(self(), worker);
        }
    }

    void on_begin_teardown(const begin_teardown& msg) {
        m_requester = msg.sender;

        // Don't block this worker thread: completion arrives as termination_msg
        cas::stop_config config;
        config.wait_for_stop = false;
        for (auto& worker : m_workers) {
            cas::system::stop_actor(worker, config);
        }
    }

    void on_worker_stopped(const cas::termination_msg& msg) {
        m_workers_stopped++;
        std::cout << "Worker " << msg.actor_name << " stopped ("
                  << m_workers_stopped << "/" << m_workers.size() << ")" << std::endl;

        if (m_workers_stopped == m_workers.size() && m_requester.is_valid()) {
            m_requester.tell(teardown_complete{});
        }
    }
};
```

The code that sends `begin_teardown` (for example another actor) calls
`system::shutdown()` once it receives `teardown_complete`.

## Debugging Lifecycle Issues

### Trace Lifecycle Calls

```cpp
class traced_actor : public cas::actor {
protected:
    void on_start() override {
        std::cout << "[" << name() << "] on_start()" << std::endl;
        handler<msg>(&traced_actor::on_msg);
    }

    void on_shutdown() override {
        std::cout << "[" << name() << "] on_shutdown()" << std::endl;
    }

    void on_stop() override {
        std::cout << "[" << name() << "] on_stop()" << std::endl;
    }

    void on_msg(const msg& m) {
        std::cout << "[" << name() << "] on_msg()" << std::endl;
    }
};
```

### Check Shutdown Log

```cpp
cas::system::shutdown();
cas::system::wait_for_shutdown();

// Get warnings about undrained messages
auto log = cas::system::get_shutdown_log();
for (const auto& warning : log) {
    std::cerr << "Warning: " << warning << std::endl;
}
```

## Next Steps

- [Message Passing](40_message_passing.md) - Master message handling
- [Actor Registry](60_actor_registry.md) - Name-based actor discovery
- [Dynamic Removal](70_dynamic_removal.md) - Stop actors at runtime
- [Timers](90_timers.md) - Schedule messages for later delivery
