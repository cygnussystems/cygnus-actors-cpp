#ifndef CAS_ZEROMQ_RELAY_ACTOR_H
#define CAS_ZEROMQ_RELAY_ACTOR_H

#ifdef CAS_ENABLE_ZEROMQ

#include "actor.h"
#include "actor_ref.h"
#include "detail/transport.h"
#include <vector>
#include <unordered_map>
#include <string>
#include <memory>
#include <chrono>

namespace cas {

// Message types for ZeroMQ relay communication
namespace msg {

// Connect to remote relay (DEALER mode - client)
struct zeromq_connect : public cas::message_base {
    std::string endpoint;       // "tcp://host:port" or "ipc:///tmp/relay"
    std::string identity;       // Optional client identity
};

// Bind to endpoint (ROUTER mode - server)
struct zeromq_bind : public cas::message_base {
    std::string endpoint;       // "tcp://*:port" or "ipc:///tmp/relay"
};

// Send raw bytes to remote
struct zeromq_send : public cas::message_base {
    std::string target;             // ROUTER: which client, DEALER: ignored
    uint64_t correlation_id = 0;    // For request/reply matching
    std::vector<char> data;         // Raw payload - FIX, JSON, binary, etc.

    // Optional hints (application-defined, passed through on receive)
    std::string content_type;       // "fix", "json", "protobuf", "raw"
    std::string message_type;       // Routing hint (e.g., "NewOrderSingle")
};

// Received raw bytes from remote
struct zeromq_recv : public cas::message_base {
    std::string source;             // ROUTER: client identity, DEALER: empty
    uint64_t correlation_id = 0;    // Matches send if reply
    std::vector<char> data;         // Raw payload

    std::string content_type;
    std::string message_type;
};

// Routing rule: forward received messages to target actor
struct zeromq_route : public cas::message_base {
    std::string message_type;       // Match this message_type
    cas::actor_ref target;          // Forward to this actor
};

// Configuration
struct zeromq_config : public cas::message_base {
    int poll_interval_ms = 10;          // How often to check for messages
    int send_timeout_ms = 1000;         // Send timeout; also the linger on close
    int recv_timeout_ms = 1000;         // No effect: receives are non-blocking
    int max_message_size = 1048576;     // 1MB max
    bool auto_reconnect = true;         // Reconnect after a dropped connection (ZMQ_RECONNECT_IVL)
    int reconnect_delay_ms = 1000;      // Delay before reconnecting
    int request_timeout_ms = 30000;     // Forget an unanswered correlation id after this
};

// Status update
struct zeromq_status : public cas::message_base {
    bool connected = false;
    std::string endpoint;
    std::string mode;                  // "router" or "dealer"
    size_t pending_count = 0;          // Outstanding correlation IDs
    size_t route_count = 0;            // Number of routing rules
};

// Error notification
struct zeromq_error : public cas::message_base {
    std::string message;
    std::string endpoint;
    int error_code = 0;
};

// Disconnect notification
struct zeromq_disconnect : public cas::message_base {
    std::string endpoint;
    std::string reason;
};

} // namespace msg

// ZeroMQ relay actor for inter-process actor communication
// Supports ROUTER (server) and DEALER (client) modes
class zeromq_relay_actor : public cas::actor {
public:
    enum class mode {
        router,  // Server - accepts connections from multiple clients
        dealer   // Client - connects to a router
    };

protected:
    void on_start() override;
    void on_stop() override;

    // Message handlers
    void on_connect(const msg::zeromq_connect& msg);
    void on_bind(const msg::zeromq_bind& msg);
    void on_send(const msg::zeromq_send& msg);
    void on_route(const msg::zeromq_route& msg);
    void on_config(const msg::zeromq_config& msg);
    void on_status_request(const message_base& msg);

    // Poll for incoming ZeroMQ messages (called by timer)
    void poll();

    // Override to interleave ZeroMQ polling with actor messages
    void process_next_message() override;

    // Send error notification to the routes, and to reply_to if it is valid
    // (the sender of the control message that failed)
    void send_error(const std::string& message, int error_code = 0,
                    const cas::actor_ref& reply_to = cas::actor_ref());

public:
    zeromq_relay_actor();
    ~zeromq_relay_actor() override;

private:
    // The connection. The relay is the actor side - routing, correlation,
    // errors, status; the transport moves the bytes (detail::transport,
    // detail::zeromq_transport). Created by bind/connect, released on stop.
    std::unique_ptr<detail::transport> m_transport;
    detail::transport_options m_options;
    mode m_mode = mode::router;
    std::atomic<bool> m_connected{false};
    std::string m_endpoint;

    // Relay configuration (transport settings are in m_options)
    int m_poll_interval_ms = 10;
    int m_request_timeout_ms = 30000;

    // Timer for polling
    timer_id m_poll_timer = INVALID_TIMER_ID;

    // Routing table: message_type -> actor_ref
    std::unordered_map<std::string, cas::actor_ref> m_routes;

    // Pending requests for correlation (correlation_id -> sender), expired
    // after m_request_timeout_ms by the poll tick
    struct pending_request {
        cas::actor_ref requester;
        std::chrono::steady_clock::time_point deadline;
    };
    std::unordered_map<uint64_t, pending_request> m_pending_requests;

    // Reused receive buffers
    std::vector<detail::envelope> m_inbound;
    std::vector<detail::transport_event> m_events;

    void expire_pending_requests();
    void start_polling();
    void report_transport_events();

    // Default route (for unmatched messages)
    cas::actor_ref m_default_route;

    // Helper to route received message
    void route_message(detail::envelope& message);

    // Release the connection and forget pending requests
    void cleanup_socket();
};

} // namespace cas

#endif // CAS_ENABLE_ZEROMQ

#endif // CAS_ZEROMQ_RELAY_ACTOR_H
