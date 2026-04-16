# Message Passing

Message passing is the core communication mechanism in Cygnus. Actors send immutable messages to each other, avoiding shared mutable state and making concurrent programming safer and more intuitive.

## Quick Example

```cpp
// Define messages
struct work_request : public cas::message_base {
    int task_id;
    std::string data;
};

struct work_response : public cas::message_base {
    int task_id;
    bool success;
    std::string result;
};

// Worker actor
class worker : public cas::actor {
protected:
    void on_start() override {
        set_name("worker");
        handler<work_request>(&worker::on_work);
    }

    void on_work(const work_request& msg) {
        // Process work
        std::string result = process(msg.data);

        // Reply to sender
        if (msg.sender.is_valid()) {
            work_response response;
            response.task_id = msg.task_id;
            response.success = true;
            response.result = result;
            msg.sender.tell(response);
        }
    }
};
```

## Message Definition

### Basic Structure

Messages inherit from `cas::message_base`:

```cpp
struct my_message : public cas::message_base {
    int value;
    std::string text;
    double amount;
};
```

### Message Base Fields

All messages automatically include:

```cpp
struct message_base {
    actor_ref sender;           // Who sent this message (set automatically)
    uint64_t message_id = 0;    // Unique message ID (set automatically)
    uint64_t correlation_id = 0; // ID of message this is replying to (optional)
    virtual ~message_base() = default;
};
```

**sender** - Automatically set when message sent from an actor
**message_id** - Globally unique ID assigned by framework
**correlation_id** - You set this to track request/response pairs

### Message Design Patterns

#### Simple Data Carrier

```cpp
struct tick : public cas::message_base {
    int count;
};

struct temperature_reading : public cas::message_base {
    double celsius;
    std::chrono::system_clock::time_point timestamp;
};
```

#### Request/Response Pair

```cpp
struct calculate_request : public cas::message_base {
    int a;
    int b;
    std::string operation;
};

struct calculate_response : public cas::message_base {
    int result;
    bool error;
    std::string error_message;
};
```

#### Command Messages

```cpp
struct start_processing : public cas::message_base {
    std::string filename;
};

struct stop_processing : public cas::message_base {};

struct pause_processing : public cas::message_base {
    int duration_seconds;
};
```

#### Event Notifications

```cpp
struct order_placed : public cas::message_base {
    std::string order_id;
    double amount;
    std::string customer_id;
};

struct payment_received : public cas::message_base {
    std::string transaction_id;
    double amount;
};
```

### Complex Data Types

Messages can contain any copyable data:

```cpp
struct user_data : public cas::message_base {
    std::string user_id;
    std::vector<std::string> permissions;
    std::map<std::string, std::string> attributes;
    std::optional<std::string> email;
};

struct batch_update : public cas::message_base {
    std::vector<int> ids;
    std::unordered_map<int, std::string> updates;
};
```

### Message Naming Conventions

Organize messages by namespace and use descriptive names:

```cpp
namespace message {
    struct ping : public cas::message_base {
        int id;
    };

    struct pong : public cas::message_base {
        int id;
    };

    struct shutdown : public cas::message_base {};
}

// Usage
message::ping p;
p.id = 1;
actor_ref.tell(p);
```

## Handler Registration

### Method Pointer Style

Register a member function to handle a message type:

```cpp
class my_actor : public cas::actor {
protected:
    void on_start() override {
        // Register handler for ping messages
        handler<message::ping>(&my_actor::on_ping);
        handler<message::pong>(&my_actor::on_pong);
    }

    void on_ping(const message::ping& msg) {
        std::cout << "Received ping " << msg.id << std::endl;
        // Handle message...
    }

    void on_pong(const message::pong& msg) {
        std::cout << "Received pong " << msg.id << std::endl;
    }
};
```

**Handler signature:** `void method_name(const MessageType& msg)`

### Lambda Style

Register an inline lambda handler:

```cpp
class my_actor : public cas::actor {
protected:
    void on_start() override {
        // Lambda handler
        handler<tick>([this](const tick& msg) {
            m_tick_count++;
            std::cout << "Tick " << m_tick_count << std::endl;
        });

        // Lambda calling member function
        handler<work_request>([this](const work_request& msg) {
            process_work(msg);
        });
    }

private:
    int m_tick_count = 0;

    void process_work(const work_request& msg) {
        // Implementation...
    }
};
```

**When to use lambdas:**
- Simple one-line handlers
- Capturing state for closures
- Delegating to private methods
- Inline message processing

### Multiple Handlers per Actor

Actors can handle many message types:

```cpp
class server_actor : public cas::actor {
protected:
    void on_start() override {
        handler<connect_msg>(&server_actor::on_connect);
        handler<disconnect_msg>(&server_actor::on_disconnect);
        handler<data_msg>(&server_actor::on_data);
        handler<ping_msg>(&server_actor::on_ping);
        handler<shutdown_msg>(&server_actor::on_shutdown);
    }

    void on_connect(const connect_msg& msg) { /* ... */ }
    void on_disconnect(const disconnect_msg& msg) { /* ... */ }
    void on_data(const data_msg& msg) { /* ... */ }
    void on_ping(const ping_msg& msg) { /* ... */ }
    void on_shutdown(const shutdown_msg& msg) { /* ... */ }
};
```

### Handler Registration Rules

**DO:**
- Register handlers in `on_start()`
- Register one handler per message type
- Use descriptive handler method names

**DON'T:**
- Register handlers in constructor (system not ready yet)
- Register multiple handlers for same message type (last one wins)
- Modify handler registrations after `on_start()`

## Sending Messages

### Basic Sending

#### tell() Method

Primary API for sending messages:

```cpp
actor_ref target = cas::actor_registry::get("worker");

work_request msg;
msg.task_id = 123;
msg.data = "process this";

target.tell(msg);  // Enqueue message, returns immediately
```

#### Stream Operator

Convenient alternative syntax:

```cpp
work_request msg;
msg.task_id = 456;
msg.data = "process that";

target << msg;  // Same as target.tell(msg)
```

#### Chaining

```cpp
actor_ref worker1 = cas::actor_registry::get("worker1");
actor_ref worker2 = cas::actor_registry::get("worker2");

work_request msg{123, "data"};

worker1 << msg;
worker2 << msg;  // Send same message to multiple actors
```

### Sending from Main Thread

Messages sent from non-actor context have invalid sender:

```cpp
int main() {
    auto actor = cas::system::create<worker>();
    cas::system::start();

    work_request msg;
    msg.task_id = 1;
    actor.tell(msg);  // msg.sender will be INVALID

    // Worker cannot reply (no sender)
}
```

### Sending from Actors

Messages sent from actors automatically have sender set:

```cpp
class requester : public cas::actor {
private:
    cas::actor_ref m_worker;

protected:
    void on_start() override {
        set_name("requester");
        handler<work_response>(&requester::on_response);

        m_worker = cas::actor_registry::get("worker");

        // Send request
        work_request msg;
        msg.task_id = 1;
        m_worker.tell(msg);  // msg.sender = this actor automatically
    }

    void on_response(const work_response& msg) {
        std::cout << "Got response: " << msg.result << std::endl;
    }
};
```

## Reply-to Pattern

### Basic Reply

Use `msg.sender` to reply to the sender:

```cpp
void on_request(const request& msg) {
    // Process request
    std::string result = process(msg.data);

    // Reply to sender
    if (msg.sender.is_valid()) {
        response resp;
        resp.data = result;
        msg.sender.tell(resp);
    }
}
```

### Check Sender Validity

Always check before replying:

```cpp
void on_work(const work_request& msg) {
    if (msg.sender.is_valid()) {
        // Reply possible
        work_response resp;
        resp.success = true;
        msg.sender.tell(resp);
    } else {
        // Message came from main thread or non-actor
        // Cannot reply - log it or handle differently
        std::cerr << "Warning: No sender to reply to" << std::endl;
    }
}
```

### Request-Response Pattern

Complete example with correlation:

```cpp
class client : public cas::actor {
private:
    cas::actor_ref m_server;
    uint64_t m_next_request_id = 1;
    std::map<uint64_t, std::string> m_pending_requests;

protected:
    void on_start() override {
        set_name("client");
        handler<response_msg>(&client::on_response);

        m_server = cas::actor_registry::get("server");

        // Send request
        send_request("get_user_123");
    }

    void send_request(const std::string& query) {
        request_msg msg;
        msg.message_id = m_next_request_id++;
        msg.query = query;

        m_pending_requests[msg.message_id] = query;
        m_server.tell(msg);
    }

    void on_response(const response_msg& msg) {
        // Match response to request using correlation_id
        auto it = m_pending_requests.find(msg.correlation_id);
        if (it != m_pending_requests.end()) {
            std::cout << "Response for '" << it->second
                      << "': " << msg.data << std::endl;
            m_pending_requests.erase(it);
        }
    }
};

class server : public cas::actor {
protected:
    void on_start() override {
        set_name("server");
        handler<request_msg>(&server::on_request);
    }

    void on_request(const request_msg& msg) {
        // Process request
        std::string result = query_database(msg.query);

        // Reply with correlation
        if (msg.sender.is_valid()) {
            response_msg resp;
            resp.correlation_id = msg.message_id;  // Link to request
            resp.data = result;
            msg.sender.tell(resp);
        }
    }
};
```

## Message Delivery Semantics

### At-Most-Once Delivery

Messages are delivered **at most once** - they may be lost if:
- Target actor is stopped
- System is shutting down
- Target actor in `stopping` state

```cpp
actor.tell(msg);  // May be dropped if actor stopped
```

### FIFO Order

Messages from actor A to actor B are delivered in send order:

```cpp
// In actor A
target.tell(msg1);
target.tell(msg2);
target.tell(msg3);

// Actor B receives: msg1, then msg2, then msg3 (guaranteed order)
```

### No Global Ordering

Messages from different senders may interleave:

```cpp
// Actor A sends: msg_a1, msg_a2
// Actor B sends: msg_b1, msg_b2

// Target actor C might tell:
// msg_a1, msg_b1, msg_a2, msg_b2  (interleaved)
// or msg_a1, msg_a2, msg_b1, msg_b2  (grouped)
// or any other valid interleaving
```

### Asynchronous Delivery

`tell()` returns immediately - message is queued:

```cpp
auto start = std::chrono::steady_clock::now();

actor.tell(msg);  // Returns in microseconds

auto duration = std::chrono::steady_clock::now() - start;
// duration is typically < 10 microseconds
```

The message is processed later by the target actor's worker thread.

## Sender Tracking

### Automatic Sender Detection

Framework sets `msg.sender` automatically:

```cpp
class actor_a : public cas::actor {
protected:
    void on_start() override {
        auto b = cas::actor_registry::get("actor_b");

        my_message msg;
        b.tell(msg);
        // Framework automatically sets msg.sender = this actor
    }
};
```

### Thread-Local Context

Sender is tracked using thread-local storage:

```cpp
// Framework internal (you don't call this)
thread_local actor* current_actor_context = nullptr;

// When actor processes message:
current_actor_context = this_actor;
dispatch_message(msg);
current_actor_context = nullptr;
```

### Sender from Main Thread

```cpp
int main() {
    auto actor = cas::system::create<my_actor>();
    cas::system::start();

    my_message msg;
    actor.tell(msg);
    // msg.sender is INVALID (not sent from an actor)

    cas::system::shutdown();
    cas::system::wait_for_shutdown();
}
```

### Forwarding Messages

Preserve original sender when forwarding:

```cpp
class proxy : public cas::actor {
private:
    cas::actor_ref m_backend;

protected:
    void on_start() override {
        handler<request>(&proxy::on_request);
        m_backend = cas::actor_registry::get("backend");
    }

    void on_request(const request& msg) {
        // Option 1: Forward with current proxy as sender
        m_backend.tell(msg);  // backend sees proxy as sender

        // Option 2: Manually preserve original sender
        // (Not directly supported - need to include sender in message)
        forward_request fwd;
        fwd.original_sender = msg.sender;
        fwd.data = msg.data;
        m_backend.tell(fwd);
    }
};
```

## Message Patterns

### Fire-and-Forget

Send without expecting a reply:

```cpp
void trigger_action() {
    notification msg;
    msg.text = "Something happened";

    auto logger = cas::actor_registry::get("logger");
    logger.tell(msg);  // Don't wait for response
}
```

### Request-Response

Send and handle reply:

```cpp
// Requester
void on_start() override {
    handler<response>(&requester::on_response);

    auto worker = cas::actor_registry::get("worker");
    request req;
    req.data = "process this";
    worker.tell(req);  // Will get response later
}

void on_response(const response& msg) {
    std::cout << "Got result: " << msg.result << std::endl;
}

// Worker
void on_request(const request& msg) {
    if (msg.sender.is_valid()) {
        response resp;
        resp.result = process(msg.data);
        msg.sender.tell(resp);
    }
}
```

### Broadcast

Send to multiple actors:

```cpp
void broadcast_update(const update& msg) {
    std::vector<cas::actor_ref> subscribers = get_subscribers();

    for (auto& subscriber : subscribers) {
        subscriber.tell(msg);
    }
}
```

### Publish-Subscribe

```cpp
class publisher : public cas::actor {
private:
    std::vector<cas::actor_ref> m_subscribers;

protected:
    void on_start() override {
        handler<subscribe>(&publisher::on_subscribe);
        handler<unsubscribe>(&publisher::on_unsubscribe);
    }

    void on_subscribe(const subscribe& msg) {
        if (msg.sender.is_valid()) {
            m_subscribers.push_back(msg.sender);
        }
    }

    void on_unsubscribe(const unsubscribe& msg) {
        // Remove sender from subscribers
        auto it = std::find(m_subscribers.begin(),
                           m_subscribers.end(),
                           msg.sender);
        if (it != m_subscribers.end()) {
            m_subscribers.erase(it);
        }
    }

    void publish_event(const event& evt) {
        for (auto& sub : m_subscribers) {
            sub.tell(evt);
        }
    }
};
```

## Performance Considerations

### Message Copying

Messages are copied when sent:

```cpp
my_message msg;
msg.data = large_string;  // 10KB string

actor.tell(msg);  // Message is COPIED to queue
// Original msg can be reused or modified
```

**Optimization:** Keep messages small, use shared_ptr for large data:

```cpp
struct large_data_msg : public cas::message_base {
    std::shared_ptr<const std::vector<uint8_t>> data;
};

// Create once
auto data = std::make_shared<std::vector<uint8_t>>(1000000);

// Send to many actors (only pointer copied)
large_data_msg msg{data};
actor1.tell(msg);  // Fast
actor2.tell(msg);  // Fast
actor3.tell(msg);  // Fast
```

### Lock-Free Queues

Message sending uses lock-free queues (moodycamel::ConcurrentQueue):
- No lock contention
- ~1-10 microsecond latency
- Scales with cores

### Batch Sending

When sending many messages:

```cpp
// ✓ GOOD: Send all at once
for (int i = 0; i < 1000; ++i) {
    work_msg msg{i};
    worker.tell(msg);  // Fast
}

// ✗ BAD: Don't interleave with slow operations
for (int i = 0; i < 1000; ++i) {
    work_msg msg{i};
    worker.tell(msg);
    std::this_thread::sleep_for(1ms);  // Slow!
}
```

## Best Practices

### DO: Make Messages Immutable

```cpp
// ✓ GOOD: Message data doesn't change
struct user_created : public cas::message_base {
    const std::string user_id;
    const std::string email;
};
```

### DO: Use Meaningful Message Names

```cpp
// ✓ GOOD: Clear intent
struct place_order : public cas::message_base { /* ... */ };
struct order_confirmed : public cas::message_base { /* ... */ };
struct payment_processed : public cas::message_base { /* ... */ };

// ✗ BAD: Generic names
struct message1 : public cas::message_base { /* ... */ };
struct data : public cas::message_base { /* ... */ };
```

### DO: Check Sender Before Replying

```cpp
void on_request(const request& msg) {
    // ✓ GOOD: Check first
    if (msg.sender.is_valid()) {
        msg.sender.tell(response{});
    }

    // ✗ BAD: Assume sender exists
    // msg.sender.tell(response{});  // Crash if invalid!
}
```

### DON'T: Send Mutable References

```cpp
// ✗ BAD: Shared mutable state
struct bad_message : public cas::message_base {
    std::vector<int>* data;  // Pointer to mutable data - DANGER!
};

// ✓ GOOD: Copy or shared immutable
struct good_message : public cas::message_base {
    std::vector<int> data;  // Copy
    // or
    std::shared_ptr<const std::vector<int>> data;  // Shared immutable
};
```

### DON'T: Block in Handlers

```cpp
void on_message(const my_msg& msg) {
    // ✗ BAD: Blocking call
    auto result = http_get("http://slow-server.com");

    // ✓ GOOD: Offload to ask pattern or separate thread
    // Use ask pattern for synchronous operations
}
```

## Debugging Tips

### Enable Logging

```cpp
void on_message(const my_msg& msg) {
    std::cout << "[" << name() << "] "
              << "Received message from "
              << (msg.sender.is_valid() ? msg.sender.name() : "main")
              << ", ID=" << msg.message_id
              << std::endl;
}
```

### Track Message Flow

```cpp
struct tracked_message : public cas::message_base {
    std::string trace_id;  // For distributed tracing
    std::vector<std::string> path;  // Actors that handled this
};

void on_tracked(const tracked_message& msg) {
    // Add self to path
    tracked_message forwarded = msg;
    forwarded.path.push_back(name());

    // Forward to next actor
    next_actor.tell(forwarded);
}
```

## Next Steps

- [Lifecycle Hooks](50_lifecycle_hooks.md) - Master actor initialization and cleanup
- [Actor Registry](60_actor_registry.md) - Name-based actor discovery
- [Ask Pattern](80_ask_pattern.md) - Synchronous request-response
- [Best Practices](120_best_practices.md) - Design patterns and anti-patterns
