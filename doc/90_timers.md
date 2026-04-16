# Timers

Timers allow actors to schedule messages for delayed or periodic delivery. This enables time-based behaviors without blocking or manual sleep loops.

## Quick Example

```cpp
struct tick : public cas::message_base {
    int count;
};

class timer_actor : public cas::actor {
private:
    cas::timer_id m_timer_id;
    int m_tick_count = 0;

protected:
    void on_start() override {
        set_name("timer_actor");
        handler<tick>(&timer_actor::on_tick);

        // Schedule tick every 100ms
        tick msg;
        msg.count = 0;
        m_timer_id = schedule_periodic(
            std::chrono::milliseconds(100),
            msg
        );
    }

    void on_tick(const tick& msg) {
        m_tick_count++;
        std::cout << "Tick #" << m_tick_count << std::endl;

        if (m_tick_count >= 10) {
            // Stop after 10 ticks
            cancel_timer(m_timer_id);
        }
    }
};
```

## Timer Types

### One-Shot Timers

Fire exactly once after a delay.

**API:**
```cpp
cas::timer_id schedule_once(
    std::chrono::milliseconds delay,
    const MessageType& message
);
```

**Example:**
```cpp
struct reminder : public cas::message_base {
    std::string text;
};

class reminder_actor : public cas::actor {
protected:
    void on_start() override {
        handler<reminder>(&reminder_actor::on_reminder);

        // Schedule reminder for 5 seconds from now
        reminder msg;
        msg.text = "Time to take a break!";

        cas::timer_id id = schedule_once(
            std::chrono::seconds(5),
            msg
        );

        std::cout << "Reminder scheduled (ID: " << id << ")" << std::endl;
    }

    void on_reminder(const reminder& msg) {
        std::cout << "REMINDER: " << msg.text << std::endl;
    }
};
```

### Periodic Timers

Fire repeatedly at fixed intervals.

**API:**
```cpp
cas::timer_id schedule_periodic(
    std::chrono::milliseconds interval,
    const MessageType& message
);
```

**Example:**
```cpp
struct heartbeat : public cas::message_base {
    std::chrono::system_clock::time_point timestamp;
};

class monitor : public cas::actor {
private:
    cas::timer_id m_heartbeat_timer;

protected:
    void on_start() override {
        handler<heartbeat>(&monitor::on_heartbeat);

        // Send heartbeat every second
        heartbeat msg;
        msg.timestamp = std::chrono::system_clock::now();

        m_heartbeat_timer = schedule_periodic(
            std::chrono::seconds(1),
            msg
        );
    }

    void on_heartbeat(const heartbeat& msg) {
        auto now = std::chrono::system_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - msg.timestamp
        );

        std::cout << "Heartbeat (system running for "
                  << elapsed.count() << "s)" << std::endl;
    }
};
```

## Timer Management

### Timer IDs

Each timer gets a unique ID:

```cpp
cas::timer_id id = schedule_once(delay, msg);
std::cout << "Timer ID: " << id << std::endl;  // e.g., 42
```

**Invalid Timer ID:**
```cpp
cas::timer_id invalid_id = cas::INVALID_TIMER_ID;  // Special value (0)

if (my_timer == cas::INVALID_TIMER_ID) {
    std::cout << "No timer active" << std::endl;
}
```

### Cancelling Timers

Stop a timer before it fires:

**API:**
```cpp
void cancel_timer(cas::timer_id id);
```

**Example:**
```cpp
class delayed_task : public cas::actor {
private:
    cas::timer_id m_task_timer = cas::INVALID_TIMER_ID;

protected:
    void on_start() override {
        handler<execute_task>(&delayed_task::on_execute);
        handler<cancel_task>(&delayed_task::on_cancel);

        // Schedule task for 10 seconds
        execute_task msg;
        m_task_timer = schedule_once(
            std::chrono::seconds(10),
            msg
        );
    }

    void on_execute(const execute_task& msg) {
        std::cout << "Executing delayed task" << std::endl;
        m_task_timer = cas::INVALID_TIMER_ID;
    }

    void on_cancel(const cancel_task& msg) {
        if (m_task_timer != cas::INVALID_TIMER_ID) {
            cancel_timer(m_task_timer);
            m_task_timer = cas::INVALID_TIMER_ID;
            std::cout << "Task cancelled" << std::endl;
        }
    }
};
```

### Multiple Timers

Actors can have multiple active timers:

```cpp
class multi_timer_actor : public cas::actor {
private:
    cas::timer_id m_fast_timer;
    cas::timer_id m_slow_timer;
    cas::timer_id m_oneshot_timer;

protected:
    void on_start() override {
        handler<fast_tick>(&multi_timer_actor::on_fast);
        handler<slow_tick>(&multi_timer_actor::on_slow);
        handler<alarm>(&multi_timer_actor::on_alarm);

        // Fast periodic timer (100ms)
        m_fast_timer = schedule_periodic(
            std::chrono::milliseconds(100),
            fast_tick{}
        );

        // Slow periodic timer (1s)
        m_slow_timer = schedule_periodic(
            std::chrono::seconds(1),
            slow_tick{}
        );

        // One-shot timer (5s)
        m_oneshot_timer = schedule_once(
            std::chrono::seconds(5),
            alarm{}
        );
    }

    void on_fast(const fast_tick& msg) {
        // Called every 100ms
    }

    void on_slow(const slow_tick& msg) {
        // Called every 1s
    }

    void on_alarm(const alarm& msg) {
        // Called once after 5s
        m_oneshot_timer = cas::INVALID_TIMER_ID;
    }
};
```

## Timer Lifecycle

### Automatic Cleanup on Shutdown

Timers are **automatically cancelled** when actors stop:

```cpp
class auto_cleanup : public cas::actor {
private:
    cas::timer_id m_timer;

protected:
    void on_start() override {
        handler<tick>(&auto_cleanup::on_tick);

        // Schedule timer
        m_timer = schedule_periodic(
            std::chrono::milliseconds(100),
            tick{}
        );
    }

    void on_shutdown() override {
        // Timer automatically cancelled - no need to call cancel_timer()
        std::cout << "Shutting down (timer auto-cancelled)" << std::endl;
    }
};
```

**Behavior:**
- System shutdown → all timers cancelled
- Individual actor stop → that actor's timers cancelled
- No timer fires after actor stops

### Manual Cleanup in on_shutdown()

You can explicitly cancel timers if needed:

```cpp
void on_shutdown() override {
    if (m_timer != cas::INVALID_TIMER_ID) {
        cancel_timer(m_timer);
        m_timer = cas::INVALID_TIMER_ID;
    }

    std::cout << "Timers manually cancelled" << std::endl;
}
```

### Timer Tracking

Actors internally track active timers:

```cpp
// Framework internal (you don't access this directly)
std::set<timer_id> m_active_timers;  // All active timers for this actor
```

When actor stops, framework cancels all timers in this set.

## Common Patterns

### Timeout Pattern

Implement timeouts for operations:

```cpp
struct request : public cas::message_base {
    int id;
};

struct response : public cas::message_base {
    int id;
    std::string data;
};

struct timeout : public cas::message_base {
    int request_id;
};

class client : public cas::actor {
private:
    cas::actor_ref m_server;
    std::map<int, cas::timer_id> m_request_timeouts;
    int m_next_request_id = 1;

protected:
    void on_start() override {
        handler<response>(&client::on_response);
        handler<timeout>(&client::on_timeout);

        m_server = cas::actor_registry::get("server");

        // Send request with timeout
        send_request_with_timeout();
    }

    void send_request_with_timeout() {
        int req_id = m_next_request_id++;

        // Send request
        request req;
        req.id = req_id;
        m_server.tell(req);

        // Schedule timeout
        timeout tmsg;
        tmsg.request_id = req_id;
        cas::timer_id timer = schedule_once(
            std::chrono::seconds(5),  // 5 second timeout
            tmsg
        );

        m_request_timeouts[req_id] = timer;
    }

    void on_response(const response& msg) {
        // Cancel timeout
        auto it = m_request_timeouts.find(msg.id);
        if (it != m_request_timeouts.end()) {
            cancel_timer(it->second);
            m_request_timeouts.erase(it);
        }

        std::cout << "Got response: " << msg.data << std::endl;
    }

    void on_timeout(const timeout& msg) {
        m_request_timeouts.erase(msg.request_id);
        std::cerr << "Request " << msg.request_id << " timed out!" << std::endl;

        // Retry or give up
        send_request_with_timeout();
    }
};
```

### Retry Pattern

Retry failed operations with exponential backoff:

```cpp
struct retry_operation : public cas::message_base {
    int attempt;
};

class retry_actor : public cas::actor {
private:
    int m_max_attempts = 5;
    cas::timer_id m_retry_timer = cas::INVALID_TIMER_ID;

protected:
    void on_start() override {
        handler<retry_operation>(&retry_actor::on_retry);

        // Start first attempt
        retry_operation msg;
        msg.attempt = 1;
        self().tell(msg);
    }

    void on_retry(const retry_operation& msg) {
        bool success = try_operation();

        if (success) {
            std::cout << "Operation succeeded on attempt "
                      << msg.attempt << std::endl;
            return;
        }

        if (msg.attempt >= m_max_attempts) {
            std::cerr << "Operation failed after "
                      << m_max_attempts << " attempts" << std::endl;
            return;
        }

        // Exponential backoff: 1s, 2s, 4s, 8s, 16s
        auto delay = std::chrono::seconds(1 << msg.attempt);

        std::cout << "Attempt " << msg.attempt << " failed, "
                  << "retrying in " << delay.count() << "s" << std::endl;

        retry_operation next_attempt;
        next_attempt.attempt = msg.attempt + 1;

        m_retry_timer = schedule_once(delay, next_attempt);
    }

    bool try_operation() {
        // Simulate operation that might fail
        return (rand() % 3) == 0;  // 33% success rate
    }
};
```

### Periodic Task Pattern

Run tasks at regular intervals:

```cpp
struct cleanup_task : public cas::message_base {};

class cache_actor : public cas::actor {
private:
    std::map<std::string, cached_value> m_cache;
    cas::timer_id m_cleanup_timer;

protected:
    void on_start() override {
        handler<cleanup_task>(&cache_actor::on_cleanup);

        // Run cleanup every 60 seconds
        m_cleanup_timer = schedule_periodic(
            std::chrono::seconds(60),
            cleanup_task{}
        );
    }

    void on_cleanup(const cleanup_task& msg) {
        auto now = std::chrono::system_clock::now();
        int removed = 0;

        for (auto it = m_cache.begin(); it != m_cache.end(); ) {
            if (it->second.is_expired(now)) {
                it = m_cache.erase(it);
                removed++;
            } else {
                ++it;
            }
        }

        std::cout << "Cleanup: removed " << removed << " expired entries" << std::endl;
    }
};
```

### Rate Limiting Pattern

Control message processing rate:

```cpp
struct process_batch : public cas::message_base {};

class rate_limited_actor : public cas::actor {
private:
    std::queue<work_item> m_pending_work;
    cas::timer_id m_batch_timer;
    const int m_batch_size = 10;

protected:
    void on_start() override {
        handler<work_item>(&rate_limited_actor::on_work_item);
        handler<process_batch>(&rate_limited_actor::on_process_batch);

        // Process batches every 100ms
        m_batch_timer = schedule_periodic(
            std::chrono::milliseconds(100),
            process_batch{}
        );
    }

    void on_work_item(const work_item& msg) {
        // Queue for later processing
        m_pending_work.push(msg);
    }

    void on_process_batch(const process_batch& msg) {
        int processed = 0;

        while (!m_pending_work.empty() && processed < m_batch_size) {
            process_work(m_pending_work.front());
            m_pending_work.pop();
            processed++;
        }

        if (processed > 0) {
            std::cout << "Processed " << processed << " items"
                      << " (" << m_pending_work.size() << " remaining)" << std::endl;
        }
    }
};
```

### Delayed Action Pattern

Delay actions until a condition is met:

```cpp
struct delayed_action : public cas::message_base {
    int action_id;
};

class delayed_actor : public cas::actor {
private:
    bool m_ready = false;
    std::vector<delayed_action> m_pending_actions;

protected:
    void on_start() override {
        handler<delayed_action>(&delayed_actor::on_delayed_action);
        handler<ready_msg>(&delayed_actor::on_ready);

        // Schedule ready signal
        schedule_once(
            std::chrono::seconds(3),
            ready_msg{}
        );
    }

    void on_delayed_action(const delayed_action& msg) {
        if (m_ready) {
            execute_action(msg);
        } else {
            // Not ready yet - queue for later
            m_pending_actions.push_back(msg);
        }
    }

    void on_ready(const ready_msg& msg) {
        m_ready = true;

        // Execute all pending actions
        for (const auto& action : m_pending_actions) {
            execute_action(action);
        }

        m_pending_actions.clear();
    }

    void execute_action(const delayed_action& msg) {
        std::cout << "Executing action " << msg.action_id << std::endl;
    }
};
```

## Timer Precision

### Accuracy

Timer precision depends on platform and system load:

- **Typical accuracy**: ±1-5 milliseconds
- **Minimum reliable interval**: ~10 milliseconds
- **Sub-millisecond timers**: Not recommended

```cpp
// ✓ GOOD: Reasonable intervals
schedule_periodic(std::chrono::milliseconds(100), msg);   // 100ms
schedule_periodic(std::chrono::seconds(1), msg);          // 1s

// ⚠ CAUTION: Very short intervals
schedule_periodic(std::chrono::milliseconds(1), msg);     // May be inaccurate

// ✗ BAD: Sub-millisecond (use fast_actor instead)
schedule_periodic(std::chrono::microseconds(500), msg);   // Not supported
```

### Timer Resolution

Framework uses steady_clock for timing:

```cpp
std::chrono::steady_clock::now()  // Monotonic, not affected by system clock changes
```

**Benefits:**
- Not affected by system clock adjustments
- Monotonic (never goes backward)
- Consistent across platforms

## Performance Considerations

### Timer Overhead

Each timer has small overhead:
- Memory: ~64 bytes per timer
- CPU: Minimal (uses efficient priority queue)

**Guidelines:**
- Hundreds of timers: Fine
- Thousands of timers: Acceptable
- Tens of thousands: Consider redesign

### Periodic Timer Efficiency

Periodic timers are efficient:

```cpp
// ✓ GOOD: One timer, fires repeatedly
schedule_periodic(interval, msg);

// ✗ BAD: Rescheduling in handler (unnecessary overhead)
void on_tick(const tick& msg) {
    // Do work...

    // Don't do this - use schedule_periodic instead
    schedule_once(interval, tick{});
}
```

### Message Copying

Timer messages are **copied**:

```cpp
large_message msg;  // 10KB message
msg.data = huge_vector;

schedule_periodic(interval, msg);  // Message copied to timer manager
```

**Optimization:** Use shared_ptr for large data:

```cpp
struct efficient_message : public cas::message_base {
    std::shared_ptr<const std::vector<uint8_t>> data;
};

auto data = std::make_shared<std::vector<uint8_t>>(large_data);
efficient_message msg{data};

schedule_periodic(interval, msg);  // Only pointer copied
```

## Best Practices

### DO: Store Timer IDs

```cpp
class my_actor : public cas::actor {
private:
    cas::timer_id m_timer = cas::INVALID_TIMER_ID;  // ✓ Store ID

protected:
    void on_start() override {
        m_timer = schedule_periodic(interval, msg);
    }

    void stop_timer() {
        if (m_timer != cas::INVALID_TIMER_ID) {
            cancel_timer(m_timer);
            m_timer = cas::INVALID_TIMER_ID;
        }
    }
};
```

### DO: Use Appropriate Intervals

```cpp
// ✓ GOOD: Reasonable timing
schedule_once(std::chrono::seconds(5), msg);       // 5 second delay
schedule_periodic(std::chrono::milliseconds(100), msg);  // 10 Hz

// ✗ BAD: Unrealistic timing
schedule_periodic(std::chrono::microseconds(10), msg);   // Too fast
schedule_once(std::chrono::hours(1000), msg);           // Unreasonably long
```

### DO: Cancel Timers You Don't Need

```cpp
void on_complete(const complete& msg) {
    // Job done, cancel periodic timer
    if (m_timer != cas::INVALID_TIMER_ID) {
        cancel_timer(m_timer);
        m_timer = cas::INVALID_TIMER_ID;
    }
}
```

### DON'T: Schedule Timers in Tight Loops

```cpp
// ✗ BAD: Creating many one-shot timers
for (int i = 0; i < 1000; ++i) {
    schedule_once(
        std::chrono::milliseconds(i),
        task{i}
    );
}

// ✓ GOOD: Use one timer and schedule sequentially
void on_task_complete(const task_complete& msg) {
    if (m_current_task < 1000) {
        schedule_once(
            std::chrono::milliseconds(1),
            task{m_current_task++}
        );
    }
}
```

### DON'T: Rely on Exact Timing

```cpp
// ✗ BAD: Assuming exact timing
int tick_count = 0;
void on_tick(const tick& msg) {
    tick_count++;
    // After 10 seconds, tick_count should be exactly 100
    // WRONG! Could be 98-102 due to timing variance
}

// ✓ GOOD: Use actual time
auto start_time = std::chrono::steady_clock::now();
void on_tick(const tick& msg) {
    auto elapsed = std::chrono::steady_clock::now() - start_time;
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(elapsed);
    // Use elapsed time, not tick count
}
```

## Debugging Timers

### Log Timer Activity

```cpp
void on_start() override {
    handler<tick>(&my_actor::on_tick);

    auto id = schedule_periodic(
        std::chrono::seconds(1),
        tick{}
    );

    std::cout << "Timer scheduled: ID=" << id << std::endl;
}

void on_tick(const tick& msg) {
    std::cout << "Timer fired at "
              << std::chrono::system_clock::now() << std::endl;
}

void on_shutdown() override {
    std::cout << "Timers auto-cancelled on shutdown" << std::endl;
}
```

### Track Active Timers

```cpp
class tracked_timer_actor : public cas::actor {
private:
    std::map<std::string, cas::timer_id> m_timers;

protected:
    void add_timer(const std::string& name, cas::timer_id id) {
        m_timers[name] = id;
        std::cout << "Timer '" << name << "' added (ID=" << id << ")" << std::endl;
    }

    void remove_timer(const std::string& name) {
        auto it = m_timers.find(name);
        if (it != m_timers.end()) {
            cancel_timer(it->second);
            m_timers.erase(it);
            std::cout << "Timer '" << name << "' removed" << std::endl;
        }
    }

    void on_shutdown() override {
        std::cout << "Active timers at shutdown: " << m_timers.size() << std::endl;
    }
};
```

## Next Steps

- [Message Passing](40_message_passing.md) - Core messaging concepts
- [Lifecycle Hooks](50_lifecycle_hooks.md) - Actor initialization and cleanup
- [Advanced Actors](100_advanced_actors.md) - Fast actors for high-frequency timing
- [Best Practices](120_best_practices.md) - Design patterns and guidelines
