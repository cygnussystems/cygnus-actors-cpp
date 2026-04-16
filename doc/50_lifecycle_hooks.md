# Actor Lifecycle Hooks

Actors have a well-defined lifecycle with three hooks that give you precise control over initialization, shutdown preparation, and final cleanup.

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
│ on_shutdown()│  Shutdown initiated, can still send messages
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

## The Three Hooks

### on_start()

Called when `system::start()` is invoked, before any messages are processed.

**Signature:**
```cpp
protected:
    void on_start() override;
```

**Purpose:**
- Initialize actor state
- Register message handlers
- Lookup other actors in registry
- Set actor name
- Start timers
- Send initial messages

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

Called after message draining is complete, just before actor is destroyed.

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
- **NO message sending** (messages will be dropped)

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

        // DON'T send messages here - they will be dropped!
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
- ✗ Send messages (will be silently dropped)
- ✗ Expect other actors to be available
- ✗ Block (should complete quickly)

## Call Order Guarantees

### System-Wide Shutdown

When `system::shutdown()` is called:

1. **All actors tell shutdown signal**
2. **All `on_shutdown()` hooks called** (can send messages to each other)
3. **Message draining period** (remaining messages processed)
4. **All `on_stop()` hooks called** (no message sending)
5. **Threads joined and system stopped**

```cpp
cas::system::shutdown();        // 1. Signal shutdown
                                // 2. All on_shutdown() called
                                // 3. Messages drained
cas::system::wait_for_shutdown(); // 4. All on_stop() called
                                // 5. Threads joined
```

### Individual Actor Stop

When `system::stop_actor()` is called:

1. **Actor state set to `stopping`**
2. **Timers cancelled**
3. **Message draining** (if configured)
4. **`on_shutdown()` called**
5. **`on_stop()` called**
6. **Actor removed from system**

```cpp
cas::system::stop_actor(actor_ref);
// 1-6 happen synchronously if wait_for_stop=true
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
- New messages are **rejected** (dropped)
- Actor can send messages from `on_shutdown()`
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
    auto counter = cas::system::create<counter>();
    cas::system::start();

    // Send 10 messages
    for (int i = 0; i < 10; ++i) {
        counter.tell(increment{});
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
- Remaining messages are **discarded**
- Shutdown proceeds
- Warning logged

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
        // Registry not ready yet!
        m_dependency = cas::actor_registry::get("other");  // Returns invalid ref
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
    // ✗ BAD: Messages will be dropped
    m_logger.tell(log_msg{"Stopping"});  // DROPPED!

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
class stateful_actor : public cas::actor {
private:
    std::map<std::string, int> m_state;
    std::string m_state_file;

protected:
    void on_start() override {
        m_state_file = "state_" + name() + ".dat";

        // Load persisted state
        load_state_from_file(m_state_file);

        handler<update>(&stateful_actor::on_update);
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

```cpp
class coordinator : public cas::actor {
private:
    std::vector<cas::actor_ref> m_workers;
    int m_workers_stopped = 0;

protected:
    void on_start() override {
        handler<worker_stopped>(&coordinator::on_worker_stopped);

        // Create workers and watch them
        for (int i = 0; i < 5; ++i) {
            auto worker = cas::system::create<worker_actor>();
            m_workers.push_back(worker);
            cas::system::watch(self(), worker);
        }
    }

    void on_shutdown() override {
        // Tell all workers to stop
        for (auto& worker : m_workers) {
            cas::system::stop_actor(worker);
        }
    }

    void on_worker_stopped(const cas::termination_msg& msg) {
        m_workers_stopped++;
        std::cout << "Worker " << msg.actor_name << " stopped ("
                  << m_workers_stopped << "/" << m_workers.size() << ")" << std::endl;

        if (m_workers_stopped == m_workers.size()) {
            std::cout << "All workers stopped" << std::endl;
        }
    }
};
```

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
