# Best Practices and Design Patterns

This guide covers recommended patterns, common pitfalls, and performance optimization strategies for the Cygnus Actor Framework.

## Core Principles

### 1. No Shared State

**DO:** Encapsulate state within actors
```cpp
class counter : public cas::actor {
private:
    int m_count = 0;  // ✓ Private state, accessed only by this actor

protected:
    void on_increment(const increment& msg) {
        m_count++;  // Safe: single-threaded access
    }
};
```

**DON'T:** Share state between actors
```cpp
// ✗ WRONG: Shared state requires locking, defeats actor model
std::map<std::string, int> g_shared_cache;  // DON'T DO THIS!

class actor_a : public cas::actor {
    void on_message(const msg& m) {
        g_shared_cache[m.key] = m.value;  // Race condition!
    }
};

class actor_b : public cas::actor {
    void on_message(const query& q) {
        auto value = g_shared_cache[q.key];  // Race condition!
    }
};
```

**Instead:** Use message passing
```cpp
// ✓ GOOD: Cache actor owns state, others send messages
class cache_actor : public cas::actor {
private:
    std::map<std::string, int> m_cache;  // Owned by this actor

protected:
    void on_put(const put& msg) {
        m_cache[msg.key] = msg.value;
    }

    void on_get(const get& msg) {
        reply_msg response;
        response.value = m_cache.at(msg.key);
        msg.sender.tell(response);
    }
};
```

### 2. Non-Blocking Message Handlers

**DO:** Keep handlers fast
```cpp
void on_request(const request& msg) {
    // ✓ Fast processing
    auto result = calculate(msg.data);
    msg.sender.tell(response{result});
}
```

**DON'T:** Block in handlers
```cpp
void on_request(const request& msg) {
    // ✗ WRONG: Blocks actor thread
    std::this_thread::sleep_for(std::chrono::seconds(5));  // DON'T!

    // ✗ WRONG: Synchronous I/O blocks thread
    auto data = read_file_sync(msg.filename);  // DON'T!

    // ✗ WRONG: Network I/O blocks thread
    auto response = http_get_sync(msg.url);  // DON'T!
}
```

**Instead:** Use async operations or dedicated actors
```cpp
// Option 1: Delegate to dedicated I/O actor
void on_request(const request& msg) {
    file_reader_msg reader;
    reader.filename = msg.filename;
    reader.reply_to = self();
    m_file_reader.tell(reader);
}

// Option 2: Use timers for delays
void on_request(const request& msg) {
    schedule_once(std::chrono::seconds(5), delayed_msg{msg.data});
}

// Option 3: Use fast_actor for tight loops with yielding
class io_actor : public cas::fast_actor {
    void on_read(const read& msg) {
        // Fast actor can do I/O without blocking pool threads
        auto data = read_file(msg.filename);
        msg.sender.tell(data_msg{data});
    }
};
```

### 3. Use Lifecycle Hooks Correctly

**DO:** Initialize in `on_start()`
```cpp
class service : public cas::actor {
private:
    cas::actor_ref m_dependency;
    std::string m_config;

protected:
    void on_start() override {
        // ✓ Set name
        set_name("service");

        // ✓ Register handlers
        handler<request>(&service::on_request);

        // ✓ Lookup dependencies
        m_dependency = cas::actor_registry::get("dependency");

        // ✓ Load configuration
        m_config = load_config();

        // ✓ Send initial messages
        if (m_dependency.is_valid()) {
            m_dependency.tell(ready_msg{});
        }
    }
};
```

**DON'T:** Initialize in constructor
```cpp
class service : public cas::actor {
private:
    cas::actor_ref m_dependency;

public:
    service() {
        // ✗ WRONG: Registry not ready yet
        m_dependency = cas::actor_registry::get("dependency");  // Returns invalid!

        // ✗ WRONG: System not started
        set_name("service");  // May not work correctly
    }
};
```

**DO:** Clean up in `on_shutdown()` and `on_stop()`
```cpp
void on_shutdown() override {
    // ✓ Save state (can send messages)
    save_state();

    // ✓ Notify peers
    for (auto& peer : m_peers) {
        peer.tell(goodbye_msg{});
    }
}

void on_stop() override {
    // ✓ Final cleanup (no messaging)
    if (m_file.is_open()) {
        m_file.close();
    }
}
```

**DON'T:** Send messages in `on_stop()`
```cpp
void on_stop() override {
    // ✗ WRONG: Messages will be dropped
    m_logger.tell(log_msg{"Stopped"});  // Silently dropped!

    // ✓ GOOD: Use stdout instead
    std::cout << "Actor stopped" << std::endl;
}
```

## Common Design Patterns

### Pattern 1: Request-Response

Standard message exchange pattern with sender tracking.

```cpp
// Define messages
struct query : public cas::message_base {
    std::string key;
};

struct query_response : public cas::message_base {
    std::string value;
};

// Server actor
class database : public cas::actor {
private:
    std::map<std::string, std::string> m_data;

protected:
    void on_start() override {
        set_name("database");
        handler<query>(&database::on_query);
    }

    void on_query(const query& msg) {
        query_response response;
        auto it = m_data.find(msg.key);
        response.value = (it != m_data.end()) ? it->second : "";

        // Reply to sender
        if (msg.sender.is_valid()) {
            msg.sender.tell(response);
        }
    }
};

// Client actor
class client : public cas::actor {
protected:
    void on_start() override {
        handler<query_response>(&client::on_response);

        // Send query
        auto db = cas::actor_registry::get("database");
        query q;
        q.key = "user_name";
        db.tell(q);  // sender automatically set
    }

    void on_response(const query_response& msg) {
        std::cout << "Got value: " << msg.value << std::endl;
    }
};
```

### Pattern 2: Worker Pool

Distribute work across multiple worker actors.

```cpp
struct task : public cas::message_base {
    int task_id;
    std::string data;
};

struct result : public cas::message_base {
    int task_id;
    std::string output;
};

class worker : public cas::actor {
protected:
    void on_start() override {
        handler<task>(&worker::on_task);
    }

    void on_task(const task& msg) {
        // Process task
        std::string output = process(msg.data);

        // Send result back to coordinator
        result r;
        r.task_id = msg.task_id;
        r.output = output;
        msg.sender.tell(r);
    }
};

class coordinator : public cas::actor {
private:
    std::vector<cas::actor_ref> m_workers;
    int m_next_worker = 0;
    std::map<int, cas::actor_ref> m_pending_tasks;

protected:
    void on_start() override {
        set_name("coordinator");
        handler<task>(&coordinator::on_task);
        handler<result>(&coordinator::on_result);

        // Create worker pool
        for (int i = 0; i < 4; ++i) {
            auto worker = cas::system::create<worker>();
            worker.get_checked<::worker>().set_name("worker_" + std::to_string(i));
            m_workers.push_back(cas::actor_registry::get("worker_" + std::to_string(i)));
        }
    }

    void on_task(const task& msg) {
        // Round-robin distribution
        m_workers[m_next_worker].tell(msg);
        m_pending_tasks[msg.task_id] = msg.sender;

        m_next_worker = (m_next_worker + 1) % m_workers.size();
    }

    void on_result(const result& msg) {
        // Forward result to original requester
        auto it = m_pending_tasks.find(msg.task_id);
        if (it != m_pending_tasks.end()) {
            it->second.tell(msg);
            m_pending_tasks.erase(it);
        }
    }
};
```

### Pattern 3: Publish-Subscribe

Broadcast messages to multiple subscribers.

```cpp
struct subscribe : public cas::message_base {
    std::string topic;
};

struct unsubscribe : public cas::message_base {
    std::string topic;
};

struct publish : public cas::message_base {
    std::string topic;
    std::string data;
};

struct event : public cas::message_base {
    std::string topic;
    std::string data;
};

class event_bus : public cas::actor {
private:
    std::map<std::string, std::set<cas::actor_ref>> m_subscribers;

protected:
    void on_start() override {
        set_name("event_bus");
        handler<subscribe>(&event_bus::on_subscribe);
        handler<unsubscribe>(&event_bus::on_unsubscribe);
        handler<publish>(&event_bus::on_publish);
    }

    void on_subscribe(const subscribe& msg) {
        m_subscribers[msg.topic].insert(msg.sender);
    }

    void on_unsubscribe(const unsubscribe& msg) {
        m_subscribers[msg.topic].erase(msg.sender);
    }

    void on_publish(const publish& msg) {
        auto it = m_subscribers.find(msg.topic);
        if (it != m_subscribers.end()) {
            event evt;
            evt.topic = msg.topic;
            evt.data = msg.data;

            // Broadcast to all subscribers
            for (const auto& subscriber : it->second) {
                subscriber.tell(evt);
            }
        }
    }
};

// Subscriber
class listener : public cas::actor {
protected:
    void on_start() override {
        handler<event>(&listener::on_event);

        // Subscribe to topics
        auto bus = cas::actor_registry::get("event_bus");
        subscribe sub;
        sub.topic = "news";
        bus.tell(sub);
    }

    void on_event(const event& msg) {
        std::cout << "Event on " << msg.topic << ": " << msg.data << std::endl;
    }
};
```

### Pattern 4: Pipeline

Chain actors for sequential processing stages.

```cpp
struct data_chunk : public cas::message_base {
    std::vector<uint8_t> data;
};

class stage_1_decode : public cas::actor {
private:
    cas::actor_ref m_next_stage;

protected:
    void on_start() override {
        set_name("decode");
        handler<data_chunk>(&stage_1_decode::on_data);
        m_next_stage = cas::actor_registry::get("validate");
    }

    void on_data(const data_chunk& msg) {
        // Decode data
        auto decoded = decode(msg.data);

        // Pass to next stage
        data_chunk next;
        next.data = decoded;
        m_next_stage.tell(next);
    }
};

class stage_2_validate : public cas::actor {
private:
    cas::actor_ref m_next_stage;

protected:
    void on_start() override {
        set_name("validate");
        handler<data_chunk>(&stage_2_validate::on_data);
        m_next_stage = cas::actor_registry::get("process");
    }

    void on_data(const data_chunk& msg) {
        // Validate
        if (validate(msg.data)) {
            m_next_stage.tell(msg);
        }
    }
};

class stage_3_process : public cas::actor {
protected:
    void on_start() override {
        set_name("process");
        handler<data_chunk>(&stage_3_process::on_data);
    }

    void on_data(const data_chunk& msg) {
        // Final processing
        store_result(msg.data);
    }
};

// Usage
void setup_pipeline() {
    auto decode = cas::system::create<stage_1_decode>();
    auto validate = cas::system::create<stage_2_validate>();
    auto process = cas::system::create<stage_3_process>();
    cas::system::start();
}
```

### Pattern 5: Supervisor (Watch Pattern)

Monitor actors and react to termination.

```cpp
class supervisor : public cas::actor {
private:
    std::vector<cas::actor_ref> m_workers;
    int m_restarts = 0;

protected:
    void on_start() override {
        set_name("supervisor");
        handler<cas::termination_msg>(&supervisor::on_worker_terminated);

        // Create and watch workers
        for (int i = 0; i < 3; ++i) {
            create_worker(i);
        }
    }

    void create_worker(int id) {
        auto worker = cas::system::create<worker_actor>();
        worker.get_checked<worker_actor>().set_name("worker_" + std::to_string(id));
        auto worker_ref = cas::actor_registry::get("worker_" + std::to_string(id));

        // Watch for termination
        cas::system::watch(self(), worker_ref);

        m_workers.push_back(worker_ref);
    }

    void on_worker_terminated(const cas::termination_msg& msg) {
        std::cout << "Worker " << msg.actor_name << " terminated" << std::endl;

        // Remove from list
        m_workers.erase(
            std::remove_if(m_workers.begin(), m_workers.end(),
                [&](const cas::actor_ref& ref) {
                    return ref.instance_id() == msg.instance_id;
                }),
            m_workers.end());

        // Restart if needed
        if (m_restarts < 10) {
            m_restarts++;
            create_worker(m_workers.size());
            std::cout << "Restarted worker" << std::endl;
        }
    }
};
```

### Pattern 6: State Machine (Stateful Actor)

Implement protocols with state transitions.

```cpp
class protocol_handler : public cas::stateful_actor {
public:
    enum class state { disconnected, authenticating, authenticated };

private:
    state m_state = state::disconnected;
    std::string m_session_id;

protected:
    void on_start() override {
        handler<connect>(&protocol_handler::on_connect);
        handler<auth>(&protocol_handler::on_auth);
        handler<auth_response>(&protocol_handler::on_auth_response);
        handler<command>(&protocol_handler::on_command);
        handler<disconnect>(&protocol_handler::on_disconnect);

        enter_disconnected();
    }

    void enter_disconnected() {
        m_state = state::disconnected;
        accept_message_type<connect>();
        reject_message_type<auth>();
        reject_message_type<auth_response>();
        reject_message_type<command>();
        reject_message_type<disconnect>();
    }

    void enter_authenticating() {
        m_state = state::authenticating;
        reject_message_type<connect>();
        reject_message_type<auth>();
        accept_message_type<auth_response>();
        reject_message_type<command>();
        accept_message_type<disconnect>();
    }

    void enter_authenticated() {
        m_state = state::authenticated;
        reject_message_type<connect>();
        reject_message_type<auth>();
        reject_message_type<auth_response>();
        accept_message_type<command>();
        accept_message_type<disconnect>();
    }

    void on_connect(const connect& msg) {
        open_connection(msg.address);
        enter_authenticating();

        // Send auth request
        auth a;
        a.username = "user";
        msg.sender.tell(a);
    }

    void on_auth_response(const auth_response& msg) {
        if (msg.success) {
            m_session_id = msg.session_id;
            enter_authenticated();
        } else {
            enter_disconnected();
        }
    }

    void on_command(const command& msg) {
        // Only processed in authenticated state
        execute_command(msg);
    }

    void on_disconnect(const disconnect& msg) {
        close_connection();
        enter_disconnected();
    }
};
```

## Anti-Patterns to Avoid

### Anti-Pattern 1: Blocking Operations

**Problem:**
```cpp
void on_request(const request& msg) {
    // ✗ Blocks entire actor (and pool thread)
    auto result = expensive_calculation();  // Takes 5 seconds
    msg.sender.tell(response{result});
}
```

**Solution:**
```cpp
// Option 1: Break into chunks with timers
void on_request(const request& msg) {
    m_current_work = msg;
    schedule_once(std::chrono::milliseconds(0), process_chunk{});
}

void on_process_chunk(const process_chunk& msg) {
    if (process_one_chunk()) {
        // More work to do
        schedule_once(std::chrono::milliseconds(0), process_chunk{});
    } else {
        // Done
        m_current_work.sender.tell(response{get_result()});
    }
}

// Option 2: Use fast_actor with yielding
class compute_actor : public cas::fast_actor {
    void on_request(const request& msg) {
        auto result = expensive_calculation();  // OK on dedicated thread
        msg.sender.tell(response{result});
    }
};
```

### Anti-Pattern 2: Circular Dependencies

**Problem:**
```cpp
class actor_a : public cas::actor {
private:
    cas::actor_ref m_b;

protected:
    void on_start() override {
        m_b = cas::actor_registry::get("b");  // Depends on B
    }
};

class actor_b : public cas::actor {
private:
    cas::actor_ref m_a;

protected:
    void on_start() override {
        m_a = cas::actor_registry::get("a");  // Depends on A (circular!)
    }
};
```

**Solution:**
```cpp
// Break cycle with mediator
class mediator : public cas::actor {
private:
    cas::actor_ref m_a;
    cas::actor_ref m_b;

protected:
    void on_start() override {
        set_name("mediator");
        m_a = cas::actor_registry::get("a");
        m_b = cas::actor_registry::get("b");
    }

    void on_request_for_a(const msg& m) {
        m_a.tell(m);
    }

    void on_request_for_b(const msg& m) {
        m_b.tell(m);
    }
};

// A and B depend on mediator, not each other
class actor_a : public cas::actor {
private:
    cas::actor_ref m_mediator;

protected:
    void on_start() override {
        m_mediator = cas::actor_registry::get("mediator");
    }
};
```

### Anti-Pattern 3: Shared Mutable State

**Problem:**
```cpp
// ✗ Global shared state
std::vector<int> g_shared_queue;  // DON'T!
std::mutex g_queue_mutex;

class producer : public cas::actor {
    void on_produce(const msg& m) {
        std::lock_guard lock(g_queue_mutex);  // Defeats actor model!
        g_shared_queue.push_back(m.value);
    }
};

class consumer : public cas::actor {
    void on_consume(const msg& m) {
        std::lock_guard lock(g_queue_mutex);  // Defeats actor model!
        if (!g_shared_queue.empty()) {
            process(g_shared_queue.front());
            g_shared_queue.erase(g_shared_queue.begin());
        }
    }
};
```

**Solution:**
```cpp
// ✓ Queue actor owns state
class queue_actor : public cas::actor {
private:
    std::vector<int> m_queue;  // Owned by actor

protected:
    void on_push(const push& msg) {
        m_queue.push_back(msg.value);
    }

    void on_pop(const pop& msg) {
        if (!m_queue.empty()) {
            pop_response response;
            response.value = m_queue.front();
            response.success = true;
            m_queue.erase(m_queue.begin());
            msg.sender.tell(response);
        } else {
            pop_response response;
            response.success = false;
            msg.sender.tell(response);
        }
    }
};
```

### Anti-Pattern 4: Missing Sender in Responses

**Problem:**
```cpp
void on_query(const query& msg) {
    query_response response;
    response.value = lookup(msg.key);

    // ✗ Forgot to check sender validity
    msg.sender.tell(response);  // May crash if sender invalid!
}
```

**Solution:**
```cpp
void on_query(const query& msg) {
    query_response response;
    response.value = lookup(msg.key);

    // ✓ Check before sending
    if (msg.sender.is_valid()) {
        msg.sender.tell(response);
    } else {
        std::cerr << "Warning: query received with invalid sender" << std::endl;
    }
}
```

### Anti-Pattern 5: Too Many Fast Actors

**Problem:**
```cpp
// ✗ Creating hundreds of fast actors
for (int i = 0; i < 1000; ++i) {
    auto worker = cas::system::create<fast_worker>();  // 1000 threads!
}
```

**Solution:**
```cpp
// ✓ Use pooled actors for bulk work
for (int i = 0; i < 1000; ++i) {
    auto worker = cas::system::create<pooled_worker>();  // Shares thread pool
}

// ✓ Reserve fast actors for critical paths only
auto critical1 = cas::system::create<fast_market_feed>();  // Justified
auto critical2 = cas::system::create<fast_execution_engine>();  // Justified
```

## Performance Optimization

### 1. Choose the Right Actor Type

```cpp
// ✓ Use pooled actors (default) for most cases
class logger : public cas::actor {
    // Async logging, no special requirements
};

// ✓ Use fast_actor for latency-critical paths
class market_feed : public cas::fast_actor {
    // High-frequency ticks, sub-μs latency needed
};

// ✓ Use inline_actor for synchronous services
class validator : public cas::inline_actor<true> {
    // Synchronous validation, direct method calls
};

// ✓ Use stateful_actor for state machines
class connection : public cas::stateful_actor {
    // States: disconnected, connecting, connected
};
```

### 2. Minimize Message Copying

**Avoid:**
```cpp
struct large_message : public cas::message_base {
    std::vector<uint8_t> data;  // Copied on send!
};

void send_data() {
    large_message msg;
    msg.data.resize(1000000);  // 1 MB
    actor_ref.tell(msg);  // Copies 1 MB!
}
```

**Better:**
```cpp
// Option 1: Use shared_ptr
struct shared_message : public cas::message_base {
    std::shared_ptr<std::vector<uint8_t>> data;
};

void send_data() {
    shared_message msg;
    msg.data = std::make_shared<std::vector<uint8_t>>(1000000);
    actor_ref.tell(msg);  // Only copies shared_ptr (cheap)
}

// Option 2: Send reference to data owned by actor
struct data_ready : public cas::message_base {
    int data_id;  // Reference to data in sender's state
};
```

### 3. Batch Operations

**Avoid:**
```cpp
// ✗ Sending many individual messages
for (int i = 0; i < 1000; ++i) {
    item msg;
    msg.value = i;
    actor_ref.tell(msg);  // 1000 queue operations
}
```

**Better:**
```cpp
// ✓ Send batch
struct batch : public cas::message_base {
    std::vector<int> values;
};

batch msg;
msg.values.reserve(1000);
for (int i = 0; i < 1000; ++i) {
    msg.values.push_back(i);
}
actor_ref.tell(msg);  // 1 queue operation
```

### 4. Use Ask Pattern Sparingly

**Avoid:**
```cpp
// ✗ Synchronous call from actor (blocks)
void on_request(const request& msg) {
    auto result = m_database.ask<query, query_response>(
        query{"key"},
        std::chrono::seconds(1));  // Blocks actor thread!

    msg.sender.tell(response{result.value});
}
```

**Better:**
```cpp
// ✓ Async request-response pattern
void on_request(const request& msg) {
    // Store original sender
    m_pending_requests[next_id()] = msg.sender;

    // Send async query
    query q;
    q.key = "key";
    m_database.tell(q);  // Non-blocking
}

void on_query_response(const query_response& msg) {
    // Find original requester
    auto it = m_pending_requests.find(msg.request_id);
    if (it != m_pending_requests.end()) {
        it->second.tell(response{msg.value});
        m_pending_requests.erase(it);
    }
}
```

### 5. Optimize Handler Registration

**Avoid:**
```cpp
// ✗ Registering handlers in constructor
class my_actor : public cas::actor {
public:
    my_actor() {
        handler<msg>(&my_actor::on_msg);  // System not ready!
    }
};
```

**Better:**
```cpp
// ✓ Register in on_start()
void on_start() override {
    handler<msg>(&my_actor::on_msg);
}

// ✓ For many message types, consider macros or helper
void on_start() override {
    register_handlers();
}

void register_handlers() {
    handler<msg_a>(&my_actor::on_a);
    handler<msg_b>(&my_actor::on_b);
    handler<msg_c>(&my_actor::on_c);
    // ...
}
```

## Testing Best Practices

### 1. Test Actor Isolation

```cpp
TEST_CASE("Actor processes messages correctly") {
    // Create system
    auto actor = cas::system::create<my_actor>();
    cas::system::start();

    // Wait for startup
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto actor_ref = cas::actor_registry::get("my_actor");
    REQUIRE(actor_ref.is_valid());

    // Send test message
    test_msg msg;
    msg.value = 42;
    actor_ref.tell(msg);

    // Wait for processing
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Verify state
    auto& a = actor.get_checked<my_actor>();
    REQUIRE(a.get_value() == 42);

    // Cleanup
    cas::system::shutdown();
    cas::system::wait_for_shutdown();
}
```

### 2. Test Message Sequences

```cpp
TEST_CASE("Actor handles message sequence") {
    auto actor = cas::system::create<stateful_test_actor>();
    cas::system::start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto ref = cas::actor_registry::get("test");

    // Send sequence
    ref.tell(msg_a{});
    ref.tell(msg_b{});
    ref.tell(msg_c{});

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Verify final state
    auto& a = actor.get_checked<stateful_test_actor>();
    REQUIRE(a.final_state() == expected_state);

    cas::system::shutdown();
    cas::system::wait_for_shutdown();
}
```

### 3. Test Error Handling

```cpp
TEST_CASE("Actor handles invalid input") {
    auto actor = cas::system::create<validator_actor>();
    cas::system::start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto ref = cas::actor_registry::get("validator");

    // Send invalid message
    invalid_msg bad;
    bad.value = -1;
    ref.tell(bad);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Verify error handling
    auto& v = actor.get_checked<validator_actor>();
    REQUIRE(v.error_count() == 1);

    cas::system::shutdown();
    cas::system::wait_for_shutdown();
}
```

### 4. Test Shutdown Behavior

```cpp
TEST_CASE("Actor cleans up on shutdown") {
    auto actor = cas::system::create<file_actor>();
    cas::system::start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto ref = cas::actor_registry::get("file");

    // Open file
    ref.tell(open_file{"test.txt"});
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Shutdown
    cas::system::shutdown();
    cas::system::wait_for_shutdown();

    // Verify file was closed (check file system state)
    REQUIRE(!file_is_locked("test.txt"));
}
```

## Architecture Recommendations

### 1. Layer Your Actors

```
┌─────────────────────────────────────┐
│  Application Layer                  │
│  (Business logic actors)            │
├─────────────────────────────────────┤
│  Service Layer                      │
│  (Database, cache, validators)      │
├─────────────────────────────────────┤
│  Infrastructure Layer               │
│  (Loggers, metrics, event bus)      │
└─────────────────────────────────────┘
```

### 2. Use Actor Hierarchies

```cpp
// Top-level coordinator
class application : public cas::actor {
private:
    cas::actor_ref m_service_layer;
    cas::actor_ref m_infra_layer;

protected:
    void on_start() override {
        // Create infrastructure
        auto logger = cas::system::create<logger_actor>();
        auto metrics = cas::system::create<metrics_actor>();

        // Create services
        auto database = cas::system::create<database_actor>();
        auto cache = cas::system::create<cache_actor>();

        // Create application actors
        auto business_logic = cas::system::create<logic_actor>();
    }
};
```

### 3. Keep Actors Focused

```cpp
// ✓ Single responsibility
class user_validator : public cas::actor {
    // Only validates users
};

class user_repository : public cas::actor {
    // Only stores/retrieves users
};

class user_notifier : public cas::actor {
    // Only sends notifications
};

// ✗ God actor
class user_manager : public cas::actor {
    // Validates, stores, retrieves, notifies... (too much!)
};
```

## Next Steps

- [Message Passing](40_message_passing.md) - Master message patterns
- [Lifecycle Hooks](50_lifecycle_hooks.md) - Proper initialization and cleanup
- [Advanced Actors](100_advanced_actors.md) - Fast, inline, and stateful actors
- [Dynamic Removal](70_dynamic_removal.md) - Runtime actor management
