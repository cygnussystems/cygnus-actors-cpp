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

```cpp
#include "cas/cas.h"

// Server process (ROUTER mode)
void run_server() {
    auto relay = cas::system::create<cas::zeromq_relay_actor>();
    auto handler = cas::system::create<message_handler>();
    
    cas::system::start();
    
    // Bind to endpoint
    relay.tell(cas::msg::zeromq_bind{"tcp://*:5555"});
    
    // Route incoming messages
    relay.tell(cas::msg::zeromq_route{
        .message_type = "tick",
        .target = handler
    });
    
    cas::system::wait_for_shutdown();
}

// Client process (DEALER mode)
void run_client() {
    auto relay = cas::system::create<cas::zeromq_relay_actor>();
    
    cas::system::start();
    
    // Connect to server
    relay.tell(cas::msg::zeromq_connect{
        .endpoint = "tcp://localhost:5555",
        .identity = "client1"
    });
    
    // Send FIX message
    std::string fix = "8=FIX.4.2|35=D|55=ES|44=5700.25|";
    relay.tell(cas::msg::zeromq_send{
        .correlation_id = 1,
        .data = {fix.begin(), fix.end()},
        .content_type = "fix",
        .message_type = "NewOrderSingle"
    });
    
    cas::system::wait_for_shutdown();
}
```

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
    int poll_interval_ms = 10;
    int send_timeout_ms = 1000;
    int recv_timeout_ms = 1000;
    int max_message_size = 1048576; // 1MB
    bool auto_reconnect = true;
    int reconnect_delay_ms = 1000;
};

// Status query/response
struct zeromq_status : public cas::message_base {
    bool connected = false;
    std::string endpoint;
    std::string mode;              // "router" or "dealer"
    size_t pending_count = 0;
    size_t route_count = 0;
};

// Error notification
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
relay.tell(cas::msg::zeromq_send{
    .target = "broker",
    .data = {fix.begin(), fix.end()},
    .content_type = "fix",
    .message_type = "D"  // NewOrderSingle
});

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
relay.tell(cas::msg::zeromq_send{
    .data = {json_str.begin(), json_str.end()},
    .content_type = "json",
    .message_type = "subscribe"
});

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
relay.tell(cas::msg::zeromq_send{
    .data = {serialized.begin(), serialized.end()},
    .content_type = "protobuf",
    .message_type = "tick"
});
```

## Routing

### Message Type Routing

```cpp
// Route by message_type field
relay.tell(cas::msg::zeromq_route{
    .message_type = "tick",
    .target = tick_handler
});

relay.tell(cas::msg::zeromq_route{
    .message_type = "order",
    .target = order_handler
});
```

### Default Route

```cpp
// Empty message_type = default route for unmatched messages
relay.tell(cas::msg::zeromq_route{
    .message_type = "",  // Empty = default
    .target = default_handler
});
```

### Correlation-Based Routing

For request/reply patterns, messages with matching `correlation_id` are routed to the original sender:

```cpp
// Send request
relay.tell(cas::msg::zeromq_send{
    .correlation_id = 12345,
    .data = request_data,
    .message_type = "query"
});

// Reply comes back with same correlation_id
// Automatically routed to original sender
```

## Endpoints

### TCP

```cpp
// Server (bind to all interfaces)
relay.tell(cas::msg::zeromq_bind{"tcp://*:5555"});

// Client (connect to specific host)
relay.tell(cas::msg::zeromq_connect{"tcp://192.168.1.100:5555"});
```

### IPC (Unix Domain Sockets / Named Pipes)

```cpp
// Linux/macOS
relay.tell(cas::msg::zeromq_bind{"ipc:///tmp/cygnus-relay"});

// Windows (uses named pipes internally)
relay.tell(cas::msg::zeromq_bind{"ipc://cygnus-relay"});
```

### In-Process

```cpp
// Same process, different threads - fastest option
relay.tell(cas::msg::zeromq_bind{"inproc://my-relay"});
```

## Configuration

```cpp
// Configure before connecting
cas::msg::zeromq_config config;
config.poll_interval_ms = 5;       // Fast polling for low latency
config.send_timeout_ms = 500;      // Fail fast on send
config.recv_timeout_ms = 500;      // Fail fast on tell
config.max_message_size = 10 * 1024 * 1024;  // 10MB max
config.auto_reconnect = true;       // Auto-reconnect on disconnect
config.reconnect_delay_ms = 100;    // Quick reconnect

relay.tell(config);
relay.tell(cas::msg::zeromq_connect{"tcp://server:5555"});
```

## Thread Safety

ZeroMQ sockets are NOT thread-safe. The relay actor solves this:

- All socket operations happen on the actor's assigned thread
- No locks needed - single-threaded access to socket
- Actor model naturally provides thread isolation

## Complete Example: FIX Gateway

```cpp
#include "cas/cas.h"
#include <iostream>

// FIX message handler
class fix_handler : public cas::actor {
private:
    cas::actor_ref m_relay;
    
protected:
    void on_start() override {
        set_name("fix_handler");
        
        // Create relay actor
        m_relay = cas::system::create<cas::zeromq_relay_actor>();
        
        // Bind as ROUTER (server)
        m_relay.tell(cas::msg::zeromq_bind{"tcp://*:5555"});
        
        // Route FIX message types
        m_relay.tell(cas::msg::zeromq_route{"D", self()});  // NewOrderSingle
        m_relay.tell(cas::msg::zeromq_route{"8", self()});  // ExecutionReport
        m_relay.tell(cas::msg::zeromq_route{"0", self()});  // Heartbeat
        
        // Default route for unknown types
        m_relay.tell(cas::msg::zeromq_route{"", self()});
        
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
        m_relay.tell(cas::msg::zeromq_send{
            .target = source,  // ROUTER: send to specific client
            .correlation_id = 0,
            .data = {exec_report.begin(), exec_report.end()},
            .content_type = "fix",
            .message_type = "8"
        });
    }
    
    void handle_heartbeat(const std::string& fix, const std::string& source) {
        // Respond to heartbeat
        std::string heartbeat = "8=FIX.4.2|35=0|";
        m_relay.tell(cas::msg::zeromq_send{
            .target = source,
            .data = {heartbeat.begin(), heartbeat.end()},
            .content_type = "fix",
            .message_type = "0"
        });
    }
};

int main() {
    auto handler = cas::system::create<fix_handler>();
    cas::system::start();
    cas::system::wait_for_shutdown();
    return 0;
}
```

## Error Handling

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
- **nlohmann-json** - Optional, for JSON payloads (vcpkg: `nlohmann-json`)

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

| Transport | Typical Latency | Use Case |
|-----------|-----------------|----------|
| **inproc://** | ~1μs | Same process, different threads |
| **ipc://** | ~10-50μs | Same machine, different processes |
| **tcp://** | ~50-500μs+ | Different machines (depends on network) |

For lowest latency:
- Use `inproc://` when possible
- Set `poll_interval_ms` low (1-5ms)
- Consider fast_actor for relay if high throughput

## See Also

- [Message Passing](40_message_passing.md) - Actor message fundamentals
- [Ask Pattern](80_ask_pattern.md) - Synchronous RPC
- [Best Practices](120_best_practices.md) - Performance tips
