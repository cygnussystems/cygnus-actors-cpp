#ifdef CAS_ENABLE_ZEROMQ

#include "cas/zeromq_relay_actor.h"
#include "cas/system.h"
#include "cas/actor_registry.h"
#include "cas/actor_ref_impl.h"
#include "cas/actor_impl.h"
#include <zmq.hpp>
#include <iostream>
#include <algorithm>
#include <mutex>
#include <string>

namespace cas {

namespace {

// Wakes the relay to read its socket
struct poll_tick : public cas::message_base {};

// Most inbound messages handled per poll: drains bursts without starving the
// relay's other messages (sends, routes) for long
constexpr int k_max_recv_per_poll = 256;

// One context for every relay in the process: inproc:// endpoints are only
// visible within a context. Lives as long as some relay holds it.
std::shared_ptr<zmq::context_t> shared_context() {
    static std::mutex mutex;
    static std::weak_ptr<zmq::context_t> cached;
    std::lock_guard<std::mutex> lock(mutex);
    auto context = cached.lock();
    if (!context) {
        context = std::make_shared<zmq::context_t>(1);
        cached = context;
    }
    return context;
}

} // namespace

void zeromq_relay_actor::on_start() {
    // The first relay takes the well-known name; later ones keep their unique
    // default name rather than taking it over. Hold the actor_ref from create().
    if (!actor_registry::exists("zeromq_relay")) {
        set_name("zeromq_relay");
    }

    // Register message handlers
    handler<msg::zeromq_connect>([this](const auto& msg) { on_connect(msg); });
    handler<msg::zeromq_bind>([this](const auto& msg) { on_bind(msg); });
    handler<msg::zeromq_send>([this](const auto& msg) { on_send(msg); });
    handler<msg::zeromq_route>([this](const auto& msg) { on_route(msg); });
    handler<msg::zeromq_config>([this](const auto& msg) { on_config(msg); });
    handler<poll_tick>([this](const poll_tick&) { expire_pending_requests(); });

    // Status request handler
    handler<msg::zeromq_status>([this](const auto& msg) {
        msg::zeromq_status status;
        status.connected = m_connected.load();
        status.endpoint = m_endpoint;
        status.mode = (m_mode == mode::router) ? "router" : "dealer";
        status.pending_count = m_pending_requests.size();
        status.route_count = m_routes.size();

        if (msg.sender.is_valid()) {
            msg.sender.tell(status);
        }
    });
}

void zeromq_relay_actor::on_stop() {
    cleanup_socket();
}

void zeromq_relay_actor::on_connect(const msg::zeromq_connect& msg) {
    if (m_connected) {
        send_error("Already connected to " + m_endpoint, 0, msg.sender);
        return;
    }

    try {
        if (!m_context) {
            m_context = shared_context();
        }

        // Create DEALER socket
        auto* socket = new zmq::socket_t(*static_cast<zmq::context_t*>(m_context.get()), ZMQ_DEALER);
        m_socket = socket;

        // Set identity (routing_id) if provided
        if (!msg.identity.empty()) {
            socket->set(zmq::sockopt::routing_id, zmq::buffer(msg.identity));
        }

        m_mode = mode::dealer;
        m_endpoint = msg.endpoint;

        configure_socket();

        // Connect
        socket->connect(msg.endpoint);
        m_connected = true;

        start_polling();

#ifdef CAS_DEBUG_LOGGING
        std::cout << "[ZEROMQ] Connected (DEALER) to " << msg.endpoint << "\n" << std::flush;
#endif

        // Send status update
        msg::zeromq_status status;
        status.connected = true;
        status.endpoint = m_endpoint;
        status.mode = "dealer";
        if (msg.sender.is_valid()) {
            msg.sender.tell(status);
        }

    } catch (const zmq::error_t& e) {
        cleanup_socket();
        send_error(std::string("Connect failed: ") + e.what(), e.num(), msg.sender);
    }
}

void zeromq_relay_actor::on_bind(const msg::zeromq_bind& msg) {
    if (m_connected) {
        send_error("Already bound to " + m_endpoint, 0, msg.sender);
        return;
    }

    try {
        if (!m_context) {
            m_context = shared_context();
        }

        // Create ROUTER socket
        auto* socket = new zmq::socket_t(*static_cast<zmq::context_t*>(m_context.get()), ZMQ_ROUTER);
        m_socket = socket;

        m_mode = mode::router;
        m_endpoint = msg.endpoint;

        configure_socket();

        // Bind
        socket->bind(msg.endpoint);
        m_connected = true;

        start_polling();

#ifdef CAS_DEBUG_LOGGING
        std::cout << "[ZEROMQ] Bound (ROUTER) to " << msg.endpoint << "\n" << std::flush;
#endif

        // Send status update
        msg::zeromq_status status;
        status.connected = true;
        status.endpoint = m_endpoint;
        status.mode = "router";
        if (msg.sender.is_valid()) {
            msg.sender.tell(status);
        }

    } catch (const zmq::error_t& e) {
        cleanup_socket();
        send_error(std::string("Bind failed: ") + e.what(), e.num(), msg.sender);
    }
}

void zeromq_relay_actor::on_send(const msg::zeromq_send& msg) {
    if (!m_connected || !m_socket) {
        send_error("Not connected", 0, msg.sender);
        return;
    }

    if (msg.data.size() > static_cast<size_t>(m_max_message_size)) {
        send_error("Message too large: " + std::to_string(msg.data.size()), 0, msg.sender);
        return;
    }

    try {
        auto* socket = static_cast<zmq::socket_t*>(m_socket);

        if (m_mode == mode::router) {
            // ROUTER: must send identity frame first
            if (msg.target.empty()) {
                send_error("ROUTER mode requires target identity", 0, msg.sender);
                return;
            }

            // Send: [identity][correlation_id][data][content_type][message_type]
            // Identity frame
            zmq::message_t identity_msg(msg.target.begin(), msg.target.end());
            socket->send(identity_msg, zmq::send_flags::sndmore);

            // Correlation ID as string
            std::string corr_str = std::to_string(msg.correlation_id);
            zmq::message_t corr_msg(corr_str.begin(), corr_str.end());
            socket->send(corr_msg, zmq::send_flags::sndmore);

            // Payload
            zmq::message_t data_msg(msg.data.begin(), msg.data.end());
            socket->send(data_msg, zmq::send_flags::sndmore);

            // Content type
            zmq::message_t type_msg(msg.content_type.begin(), msg.content_type.end());
            socket->send(type_msg, zmq::send_flags::sndmore);

            // Message type (final frame)
            zmq::message_t mtype_msg(msg.message_type.begin(), msg.message_type.end());
            socket->send(mtype_msg, zmq::send_flags::none);

        } else {
            // DEALER: just send data, no identity needed
            // Send: [correlation_id][data][content_type][message_type]
            std::string corr_str = std::to_string(msg.correlation_id);
            zmq::message_t corr_msg(corr_str.begin(), corr_str.end());
            socket->send(corr_msg, zmq::send_flags::sndmore);

            zmq::message_t data_msg(msg.data.begin(), msg.data.end());
            socket->send(data_msg, zmq::send_flags::sndmore);

            zmq::message_t type_msg(msg.content_type.begin(), msg.content_type.end());
            socket->send(type_msg, zmq::send_flags::sndmore);

            zmq::message_t mtype_msg(msg.message_type.begin(), msg.message_type.end());
            socket->send(mtype_msg, zmq::send_flags::none);
        }

        // Track pending request if correlation_id set
        if (msg.correlation_id != 0 && msg.sender.is_valid()) {
            m_pending_requests[msg.correlation_id] = pending_request{
                msg.sender,
                std::chrono::steady_clock::now() + std::chrono::milliseconds(m_request_timeout_ms)};
        }

    } catch (const zmq::error_t& e) {
        send_error(std::string("Send failed: ") + e.what(), e.num(), msg.sender);
    }
}

void zeromq_relay_actor::on_route(const msg::zeromq_route& msg) {
    if (msg.message_type.empty()) {
        // Empty message_type means default route
        m_default_route = msg.target;
    } else {
        m_routes[msg.message_type] = msg.target;
    }

#ifdef CAS_DEBUG_LOGGING
    std::cout << "[ZEROMQ] Route added: '" << msg.message_type << "' -> "
              << msg.target.name() << "\n" << std::flush;
#endif
}

void zeromq_relay_actor::on_config(const msg::zeromq_config& msg) {
    m_poll_interval_ms = msg.poll_interval_ms;
    m_send_timeout_ms = msg.send_timeout_ms;
    m_recv_timeout_ms = msg.recv_timeout_ms;
    m_max_message_size = msg.max_message_size;
    m_auto_reconnect = msg.auto_reconnect;
    m_reconnect_delay_ms = msg.reconnect_delay_ms;
    m_request_timeout_ms = msg.request_timeout_ms;

    // Reconfigure socket if connected
    if (m_socket) {
        configure_socket();
    }
}

void zeromq_relay_actor::process_next_message() {
    // First, poll for ZeroMQ messages
    if (m_connected && m_socket) {
        poll();
    }

    // Then process normal actor messages
    actor::process_next_message();
}

void zeromq_relay_actor::start_polling() {
    m_poll_timer = schedule_periodic(std::chrono::milliseconds(m_poll_interval_ms), poll_tick{});
}

void zeromq_relay_actor::expire_pending_requests() {
    if (m_pending_requests.empty()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(m_pending_requests, [now](const auto& entry) { return entry.second.deadline <= now; });
}

void zeromq_relay_actor::poll() {
    // Drain what is waiting (up to a budget), not one message per wake-up
    for (int i = 0; i < k_max_recv_per_poll && m_connected && m_socket; ++i) {
        if (!receive_one()) {
            break;
        }
    }
}

bool zeromq_relay_actor::receive_one() {
    auto* socket = static_cast<zmq::socket_t*>(m_socket);

    try {
        // Non-blocking receive - get all frames
        zmq::message_t frame;
        std::vector<std::vector<char>> frames;

        // Receive first frame
        auto result = socket->recv(frame, zmq::recv_flags::dontwait);
        if (!result.has_value()) {
            return false;  // No message available
        }

        // Store first frame
        frames.emplace_back(frame.data<char>(), frame.data<char>() + frame.size());

        // Receive remaining frames while more flag is set
        while (frame.more()) {
            result = socket->recv(frame, zmq::recv_flags::dontwait);
            if (!result.has_value()) {
                break;  // Incomplete multipart message
            }
            frames.emplace_back(frame.data<char>(), frame.data<char>() + frame.size());
        }

        // Parse frames
        // ROUTER: [identity][correlation_id][data][content_type][message_type]
        // DEALER: [correlation_id][data][content_type][message_type]

        size_t min_frames = (m_mode == mode::router) ? 5 : 4;
        if (frames.size() < min_frames) {
            send_error("Received malformed message (frame count: " + std::to_string(frames.size()) + ")");
            return true;  // consumed; keep reading
        }

        size_t idx = 0;
        std::string source;

        if (m_mode == mode::router) {
            // First frame is identity
            source = std::string(frames[0].begin(), frames[0].end());
            idx = 1;
        }

        // Parse remaining frames
        uint64_t correlation_id = 0;
        std::vector<char> data;
        std::string content_type;
        std::string message_type;

        // Correlation ID
        std::string corr_str(frames[idx].begin(), frames[idx].end());
        try {
            correlation_id = std::stoull(corr_str);
        } catch (...) {
            correlation_id = 0;
        }
        idx++;

        // Data
        data = frames[idx];
        idx++;

        // Content type
        content_type = std::string(frames[idx].begin(), frames[idx].end());
        idx++;

        // Message type
        message_type = std::string(frames[idx].begin(), frames[idx].end());

        // Route the message
        route_message(source, correlation_id, data, content_type, message_type);
        return true;

    } catch (const zmq::error_t& e) {
        // EAGAIN is expected when no messages available
        if (e.num() != EAGAIN) {
            send_error(std::string("Receive error: ") + e.what(), e.num());
        }
        return false;
    }
}

void zeromq_relay_actor::route_message(const std::string& source, uint64_t correlation_id,
                                        const std::vector<char>& data,
                                        const std::string& content_type,
                                        const std::string& message_type) {
    // Create received message
    msg::zeromq_recv recv_msg;
    recv_msg.source = source;
    recv_msg.correlation_id = correlation_id;
    recv_msg.data = data;
    recv_msg.content_type = content_type;
    recv_msg.message_type = message_type;

    // Check if this is a reply to a pending request
    if (correlation_id != 0) {
        auto it = m_pending_requests.find(correlation_id);
        if (it != m_pending_requests.end()) {
            // Send to original sender
            it->second.requester.tell(recv_msg);
            m_pending_requests.erase(it);
            return;
        }
    }

    // Route by message_type
    if (!message_type.empty()) {
        auto it = m_routes.find(message_type);
        if (it != m_routes.end() && it->second.is_valid()) {
            it->second.tell(recv_msg);
            return;
        }
    }

    // Fall back to default route
    if (m_default_route.is_valid()) {
        m_default_route.tell(recv_msg);
        return;
    }

    // No route found - log warning
#ifdef CAS_DEBUG_LOGGING
    std::cout << "[ZEROMQ] No route for message_type '" << message_type << "'\n" << std::flush;
#endif
}

void zeromq_relay_actor::configure_socket() {
    if (!m_socket) return;

    auto* socket = static_cast<zmq::socket_t*>(m_socket);

    // Set timeouts
    socket->set(zmq::sockopt::sndtimeo, m_send_timeout_ms);
    socket->set(zmq::sockopt::rcvtimeo, m_recv_timeout_ms);
    socket->set(zmq::sockopt::maxmsgsize, static_cast<int64_t>(m_max_message_size));

    // Bounded linger: the default (-1) makes the last relay's context
    // teardown wait forever for undeliverable outbound messages
    socket->set(zmq::sockopt::linger, m_send_timeout_ms);

    // ZeroMQ reconnects dropped connections by itself; -1 disables it
    socket->set(zmq::sockopt::reconnect_ivl, m_auto_reconnect ? m_reconnect_delay_ms : -1);

    // ROUTER: set router mandatory to get errors for unknown identities
    if (m_mode == mode::router) {
        socket->set(zmq::sockopt::router_mandatory, true);
    }
}

void zeromq_relay_actor::cleanup_socket() {
    if (m_poll_timer != INVALID_TIMER_ID) {
        cancel_timer(m_poll_timer);
        m_poll_timer = INVALID_TIMER_ID;
    }

    if (m_socket) {
        auto* socket = static_cast<zmq::socket_t*>(m_socket);
        delete socket;
        m_socket = nullptr;
    }

    m_context.reset();  // the last relay's release terminates the context

    m_connected = false;
    m_pending_requests.clear();
}

void zeromq_relay_actor::send_error(const std::string& message, int error_code,
                                    const cas::actor_ref& reply_to) {
    msg::zeromq_error err;
    err.message = message;
    err.endpoint = m_endpoint;
    err.error_code = error_code;

    // Broadcast to all routes, and to the requester - once each
    std::vector<const actor*> told;
    auto tell_once = [&](const cas::actor_ref& ref) {
        const actor* target = ref.get_actor().get();
        if (target && std::find(told.begin(), told.end(), target) == told.end()) {
            told.push_back(target);
            ref.tell(err);
        }
    };
    tell_once(reply_to);
    for (const auto& route : m_routes) {
        tell_once(route.second);
    }
    tell_once(m_default_route);

#ifdef CAS_DEBUG_LOGGING
    std::cout << "[ZEROMQ] Error: " << message << "\n" << std::flush;
#endif
}

} // namespace cas

#endif // CAS_ENABLE_ZEROMQ
