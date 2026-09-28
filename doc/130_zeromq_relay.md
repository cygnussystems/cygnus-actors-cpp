# ZeroMQ Relay Actor

Inter-process actor communication via ZeroMQ. The `zeromq_relay_actor` enables actor systems in separate processes (or machines) to communicate transparently through actor messages.

## Overview

```
┌──────────────────────────┐         ┌──────────────────────────┐
│  Process A               │         │  Process B               │
│  (market_simulator)      │         │  (brokerage_gateway)     │
│                          │         │                          │
│  ┌────────────────────┐  │  ZeroMQ │  ┌────────────────────┐  │
│  │ market_data_actor  │  │◄───────►│  │ order_router_actor │  │
│  └─────────┬──────────┘  │  ROUTER/ │  └─────────┬──────────┘  │
│            │             │  DEALER  │            │             │
│            ▼             │         │            ▼             │
│  ┌────────────────────┐  │         │  ┌────────────────────┐  │
│  │ zeromq_relay_actor │  │         │  │ zeromq_relay_actor │  │
│  └────────────────────┘  │         │  └────────────────────┘  │
└──────────────────────────┘         └──────────────────────────┘
```

## Quick Example

Messages derive from `cas::message_base`, which is polymorphic, so they are not
aggregates: set fields after construction (empty braces `T{}` are fine).

```cpp
#include "cas/cas.h"
#include <atomic>
#include <chrono>
#include <thread>

std::atomic<bool> g_running{true};  // cleared by your signal handler (e.g. Ctrl+C)

// Server process (ROUTER mode)
void run_server() {
    auto relay = cas::system::create<cas::zeromq_relay_actor>();
    auto handler = cas::system::create<message_handler>();

    cas::system::start();

    // Route incoming messages first: errors (e.g. a failed bind) are only
    // delivered to route targets, so a relay without routes loses them
    cas::msg::zeromq_route route;
    route.message_type = "tick";
    route.target = handler;
    relay.tell(route);

    // Bind to endpoint
    cas::msg::zeromq_bind bind;
    bind.endpoint = "tcp://*:5555";
    relay.tell(bind);

    // Keep the process alive. Do not call wait_for_shutdown() yet: it stops the
    // timer manager, and the relay polls its socket from a periodic timer
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    cas::system::shutdown();
    cas::system::wait_for_shutdown();
}

// Client process (DEALER mode)
void run_client() {
    auto relay = cas::system::create<cas::zeromq_relay_actor>();
    auto handler = cas::system::create<message_handler>();

    cas::system::start();

    // Default route (incoming messages and errors) before connecting
    cas::msg::zeromq_route route;
    route.target = handler;
    relay.tell(route);

    // Connect to server
    cas::msg::zeromq_connect connect;
    connect.endpoint = "tcp://localhost:5555";
    connect.identity = "client1";
    relay.tell(connect);

    // Send FIX message
    std::string fix = "8=FIX.4.2|35=D|55=ES|44=5700.25|";
    cas::msg::zeromq_send send;
    send.correlation_id = 1;  // not tracked: sent from main(), not an actor
    send.data.assign(fix.begin(), fix.end());
    send.content_type = "fix";
    send.message_type = "NewOrderSingle";
    relay.tell(send);

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    cas::system::shutdown();
    cas::system::wait_for_shutdown();
}
```

The first relay started takes the registry name `"zeromq_relay"`; further
relays keep their unique default names. With more than one relay, hold the
`actor_ref` returned by `create()` instead of looking relays up by name.

## ZeroMQ Patterns

### ROUTER/DEALER (Asynchronous)

The relay uses ROUTER/DEALER pattern for bidirectional async communication:

| Mode | Role | Behavior |
|------|------|----------|
| **ROUTER** | Server | Accepts multiple clients, receives identity + message |
| **DEALER** | Client | Connects to router, sends/receives asynchronously |

Why ROUTER/DEALER instead of REQ/REP:
- Multiple outstanding requests allowed
- Non-blocking operation
- ROUTER handles multiple DEALER clients

## Message Types

### Connection Messages

```cpp
namespace cas::msg {

// Connect to remote (DEALER mode - client)
struct zeromq_connect : public cas::message_base {
    std::string endpoint;       // "tcp://host:port" or "ipc:///tmp/relay"
    std::string identity;       // Optional client identity
};

// Bind to endpoint (ROUTER mode - server)
struct zeromq_bind : public cas::message_base {
    std::string endpoint;       // "tcp://*:port" or "ipc:///tmp/relay"
};

} // namespace cas::msg
```

### Data Messages

```cpp
// Send raw bytes to remote
struct zeromq_send : public cas::message_base {
    std::string target;             // ROUTER: which client, DEALER: ignored
    uint64_t correlation_id = 0;    // For request/reply matching
    std::vector<char> data;         // Raw payload - FIX, JSON, binary, etc.
    
    // Optional hints (application-defined, passed through)
    std::string content_type;       // "fix", "json", "protobuf", "raw"
    std::string message_type;       // Routing hint
};

// Received bytes from remote
struct zeromq_recv : public cas::message_base {
    std::string source;             // ROUTER: client identity, DEALER: empty
    uint64_t correlation_id = 0;    // Matches send if reply
    std::vector<char> data;         // Raw payload
    
    std::string content_type;
    std::string message_type;
};

} // namespace cas::msg
```

### Routing & Configuration

```cpp
// Routing rule: forward received messages to target actor
struct zeromq_route : public cas::message_base {
    std::string message_type;       // Match this message_type
    cas::actor_ref target;          // Forward to this actor
    // Empty message_type = default route
};

// Configuration
struct zeromq_config : public cas::message_base {
    int poll_interval_ms = 10;      // Poll timer period (see Performance)
    int send_timeout_ms = 1000;     // Send timeout; also the linger on close
    int recv_timeout_ms = 1000;     // No effect: receives are non-blocking
    int max_message_size = 1048576; // 1MB
    bool auto_reconnect = true;     // ZeroMQ reconnects dropped connections (false: never)
    int reconnect_delay_ms = 1000;  // Delay before a reconnect attempt
    int request_timeout_ms = 30000; // Forget an unanswered correlation id after this
};

// Status query/response
struct zeromq_status : public cas::message_base {
    bool connected = false;
    std::string endpoint;
    std::string mode;              // "router" or "dealer"
    size_t pending_count = 0;
    size_t route_count = 0;
};

// Error notification - sent to every route target and the default route;
// errors raised while no route exists are lost
struct zeromq_error : public cas::message_base {
    std::string message;
    std::string endpoint;
    int error_code = 0;
};

} // namespace cas::msg
```

## Payload Handling

The relay is payload-agnostic - it transports raw bytes. Application code handles serialization.

### FIX Messages (Plain Text)

```cpp
// Send FIX message
std::string fix = "8=FIX.4.2|35=D|55=ES|44=5700.25|10=123|";
cas::msg::zeromq_send send;
send.target = "broker";
send.data.assign(fix.begin(), fix.end());
send.content_type = "fix";
send.message_type = "D";  // NewOrderSingle
relay.tell(send);

// Receive FIX message
void on_zeromq_recv(const cas::msg::zeromq_recv& msg) {
    std::string fix(msg.data.begin(), msg.data.end());
    fix_message parsed = parse_fix(fix);
    // Handle...
}
```

### JSON Messages

```cpp
#include <nlohmann/json.hpp>

// Send JSON
nlohmann::json j = {
    {"type", "subscribe"},
    {"symbol", "ES"},
    {"exchange", "CME"}
};
std::string json_str = j.dump();
cas::msg::zeromq_send send;
send.data.assign(json_str.begin(), json_str.end());
send.content_type = "json";
send.message_type = "subscribe";
relay.tell(send);

// Receive JSON
void on_zeromq_recv(const cas::msg::zeromq_recv& msg) {
    if (msg.content_type == "json") {
        std::string json_str(msg.data.begin(), msg.data.end());
        auto j = nlohmann::json::parse(json_str);
        // Handle...
    }
}
```

### Binary/Protobuf

```cpp
// Send protobuf
MyMessage proto;
proto.set_symbol("ES");
proto.set_price(5700.25);
std::string serialized = proto.SerializeAsString();
cas::msg::zeromq_send send;
send.data.assign(serialized.begin(), serialized.end());
send.content_type = "protobuf";
send.message_type = "tick";
relay.tell(send);
```

## Routing

### Message Type Routing

```cpp
// Route by message_type field
cas::msg::zeromq_route tick_route;
tick_route.message_type = "tick";
tick_route.target = tick_handler;
relay.tell(tick_route);

cas::msg::zeromq_route order_route;
order_route.message_type = "order";
order_route.target = order_handler;
relay.tell(order_route);
```

### Default Route

```cpp
// Empty message_type = default route for unmatched messages
cas::msg::zeromq_route route;
route.message_type = "";  // Empty = default
route.target = default_handler;
relay.tell(route);
```

### Correlation-Based Routing

For request/reply patterns, messages with matching `correlation_id` are routed to the original sender:

```cpp
// Send request (from inside an actor, so the sender is valid)
cas::msg::zeromq_send request;
request.correlation_id = 12345;
request.data = request_data;
request.message_type = "query";
m_relay.tell(request);

// Reply comes back with same correlation_id
// Automatically routed to this actor
```

A request is tracked only if the `zeromq_send` came from inside an actor (valid
`sender`) and `correlation_id != 0`. Sends from `main()` or with
`correlation_id == 0` are not tracked; their replies go through the normal
routes. An entry whose reply has not arrived within `request_timeout_ms`
(default 30 s) is forgotten; a late reply is then routed like any other message.

## Endpoints

### TCP

```cpp
// Server (bind to all interfaces)
cas::msg::zeromq_bind bind;
bind.endpoint = "tcp://*:5555";
relay.tell(bind);

// Client (connect to specific host)
cas::msg::zeromq_connect connect;
connect.endpoint = "tcp://192.168.1.100:5555";
relay.tell(connect);
```

### IPC (Unix Domain Sockets)

```cpp
// Linux/macOS
cas::msg::zeromq_bind bind;
bind.endpoint = "ipc:///tmp/cygnus-relay";
relay.tell(bind);

// Windows: check your libzmq version's ipc:// support on Windows
```

### In-Process

All relays in a process share one ZeroMQ context, so two relays can talk over
`inproc://` (useful in tests). Between actors in one process, plain actor
messages are simpler and faster.

## Configuration

```cpp
// Configure before connecting (poll_interval_ms is read when bind/connect
// starts the poll timer)
cas::msg::zeromq_config config;
config.poll_interval_ms = 5;       // Fast polling for low latency
config.send_timeout_ms = 500;      // Fail fast on send
config.max_message_size = 10 * 1024 * 1024;  // 10MB max
config.auto_reconnect = true;       // Reconnect dropped connections...
config.reconnect_delay_ms = 100;    // ...after 100 ms
config.request_timeout_ms = 5000;   // Forget unanswered correlation ids after 5 s

relay.tell(config);

cas::msg::zeromq_connect connect;
connect.endpoint = "tcp://server:5555";
relay.tell(connect);
```

## Thread Safety

ZeroMQ sockets are NOT thread-safe. The relay actor solves this:

- All socket operations happen on the actor's assigned thread
- No locks needed - single-threaded access to socket
- Actor model naturally provides thread isolation

## Complete Example: FIX Gateway

```cpp
#include "cas/cas.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

// FIX message handler
class fix_handler : public cas::actor {
private:
    cas::actor_ref m_relay;
    
protected:
    void on_start() override {
        set_name("fix_handler");
        
        // Create relay actor
        m_relay = cas::system::create<cas::zeromq_relay_actor>();

        // Route FIX message types before binding, so bind errors reach us
        for (const char* type : {"D",    // NewOrderSingle
                                 "8",    // ExecutionReport
                                 "0",    // Heartbeat
                                 ""}) {  // Default route for unknown types
            cas::msg::zeromq_route route;
            route.message_type = type;
            route.target = self();
            m_relay.tell(route);
        }

        // Bind as ROUTER (server)
        cas::msg::zeromq_bind bind;
        bind.endpoint = "tcp://*:5555";
        m_relay.tell(bind);

        // Handle received messages
        handler<cas::msg::zeromq_recv>([this](const auto& msg) {
            on_fix_message(msg);
        });
        
        handler<cas::msg::zeromq_error>([this](const auto& msg) {
            std::cerr << "ZeroMQ error: " << msg.message << "\n";
        });
    }
    
    void on_fix_message(const cas::msg::zeromq_recv& msg) {
        std::string fix(msg.data.begin(), msg.data.end());
        std::cout << "Received FIX from " << msg.source << ": " << fix << "\n";
        
        // Parse and route by FIX MsgType (tag 35)
        if (msg.message_type == "D") {
            handle_order(fix, msg.source);
        } else if (msg.message_type == "0") {
            handle_heartbeat(fix, msg.source);
        }
    }
    
    void handle_order(const std::string& fix, const std::string& source) {
        // Process order...
        
        // Send execution report back
        std::string exec_report = "8=FIX.4.2|35=8|55=ES|44=5700.25|";
        cas::msg::zeromq_send send;
        send.target = source;  // ROUTER: send to specific client
        send.correlation_id = 0;
        send.data.assign(exec_report.begin(), exec_report.end());
        send.content_type = "fix";
        send.message_type = "8";
        m_relay.tell(send);
    }
    
    void handle_heartbeat(const std::string& fix, const std::string& source) {
        // Respond to heartbeat
        std::string heartbeat = "8=FIX.4.2|35=0|";
        cas::msg::zeromq_send send;
        send.target = source;
        send.data.assign(heartbeat.begin(), heartbeat.end());
        send.content_type = "fix";
        send.message_type = "0";
        m_relay.tell(send);
    }
};

std::atomic<bool> g_running{true};  // cleared by your signal handler

int main() {
    auto handler = cas::system::create<fix_handler>();
    cas::system::start();

    // Keep running; wait_for_shutdown() alone would stop the relay's poll timer
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    cas::system::shutdown();
    cas::system::wait_for_shutdown();
    return 0;
}
```

## Error Handling

`zeromq_error` is sent to every route target (and the default route), and to
the sender of the control message that failed - so an actor that sends
`zeromq_bind` / `zeromq_connect` / `zeromq_send` from a handler hears about its
own failures even before any route exists. (Messages sent from outside an
actor have no sender; their errors reach only the routes.)

```cpp
// Register error handler
handler<cas::msg::zeromq_error>([this](const auto& msg) {
    std::cerr << "ZeroMQ error [" << msg.error_code << "]: " << msg.message << "\n";
    std::cerr << "Endpoint: " << msg.endpoint << "\n";
    
    // Optionally attempt reconnection
    if (m_auto_reconnect) {
        // Reconnect logic...
    }
});
```

## Dependencies

ZeroMQ relay requires:
- **cppzmq** - ZeroMQ C++ binding (vcpkg: `cppzmq`)

The relay treats payloads as opaque bytes and has no JSON dependency of its
own. The JSON examples above use **nlohmann-json** (vcpkg: `nlohmann-json`);
add it to your own application if you want them - the framework does not link
it for you.

Enable in CMake:
```cmake
# Enable ZeroMQ support
option(ENABLE_ZEROMQ "Enable ZeroMQ relay actor" ON)

find_package(cppzmq CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME} PUBLIC cppzmq)
```

## Build Flag

ZeroMQ support is controlled by `CAS_ENABLE_ZEROMQ` compile definition:

```cpp
#ifdef CAS_ENABLE_ZEROMQ
#include "cas/zeromq_relay_actor.h"
// ... use relay
#endif
```

## Performance Considerations

The relay reads its socket each time it processes an actor message, draining
up to 256 waiting ZeroMQ messages at a time. It is woken by its periodic poll
timer (`poll_interval_ms`, default 10 ms) or by other messages sent to it. So:

- **Inbound latency** is up to `poll_interval_ms`, not microseconds.
- **Inbound throughput** is not limited by the tick: bursts are drained (up to
  256 messages per wake-up, then the relay's own queue gets a turn).

Outbound sends go to the socket as soon as the relay processes the `zeromq_send`.

| Transport | Use Case |
|-----------|----------|
| **ipc://** | Same machine, different processes |
| **tcp://** | Different machines (latency depends on network) |

The raw transport latency is small next to the poll interval, which dominates
inbound latency.

For lower inbound latency and higher inbound throughput:
- Set `poll_interval_ms` low (1-5ms) in a `zeromq_config` sent before bind/connect

## See Also

- [Message Passing](40_message_passing.md) - Actor message fundamentals
- [Call](80_call.md) - Synchronous RPC
- [Best Practices](120_best_practices.md) - Performance tips
