#ifdef CAS_ENABLE_ZEROMQ

#include "cas/zeromq_relay_actor.h"
#include "cas/detail/zeromq_transport.h"
#include "cas/system.h"
#include "cas/actor_registry.h"
#include "cas/actor_ref_impl.h"
#include "cas/actor_impl.h"
#include <iostream>
#include <algorithm>
#include <string>

namespace cas {

namespace {

// Wakes the relay to read its transport
struct poll_tick : public cas::message_base {};

// Most inbound messages handled per poll: drains bursts without starving the
// relay's other messages (sends, routes) for long
constexpr size_t k_max_recv_per_poll = 256;

} // namespace

zeromq_relay_actor::zeromq_relay_actor() = default;
zeromq_relay_actor::~zeromq_relay_actor() = default;

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
        m_mode = mode::dealer;
        m_endpoint = msg.endpoint;

        m_transport = std::make_unique<detail::zeromq_transport>();
        m_transport->configure(m_options);
        m_transport->connect(msg.endpoint, msg.identity);
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

    } catch (const detail::transport_error& e) {
        cleanup_socket();
        send_error(std::string("Connect failed: ") + e.what(), e.code(), msg.sender);
    }
}

void zeromq_relay_actor::on_bind(const msg::zeromq_bind& msg) {
    if (m_connected) {
        send_error("Already bound to " + m_endpoint, 0, msg.sender);
        return;
    }

    try {
        m_mode = mode::router;
        m_endpoint = msg.endpoint;

        m_transport = std::make_unique<detail::zeromq_transport>();
        m_transport->configure(m_options);
        m_transport->bind(msg.endpoint);
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

    } catch (const detail::transport_error& e) {
        cleanup_socket();
        send_error(std::string("Bind failed: ") + e.what(), e.code(), msg.sender);
    }
}

void zeromq_relay_actor::on_send(const msg::zeromq_send& msg) {
    if (!m_connected || !m_transport) {
        send_error("Not connected", 0, msg.sender);
        return;
    }

    if (msg.data.size() > static_cast<size_t>(m_options.max_message_size)) {
        send_error("Message too large: " + std::to_string(msg.data.size()), 0, msg.sender);
        return;
    }

    if (m_mode == mode::router && msg.target.empty()) {
        send_error("ROUTER mode requires target identity", 0, msg.sender);
        return;
    }

    detail::envelope out;
    out.peer = msg.target;
    out.correlation_id = msg.correlation_id;
    out.message_type = msg.message_type;
    out.content_type = msg.content_type;
    out.payload = msg.data;

    try {
        m_transport->send(out);

        // Track pending request if correlation_id set
        if (msg.correlation_id != 0 && msg.sender.is_valid()) {
            m_pending_requests[msg.correlation_id] = pending_request{
                msg.sender,
                std::chrono::steady_clock::now() + std::chrono::milliseconds(m_request_timeout_ms)};
        }

    } catch (const detail::transport_error& e) {
        send_error(std::string("Send failed: ") + e.what(), e.code(), msg.sender);
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
    m_request_timeout_ms = msg.request_timeout_ms;

    m_options.send_timeout_ms = msg.send_timeout_ms;
    m_options.recv_timeout_ms = msg.recv_timeout_ms;
    m_options.max_message_size = msg.max_message_size;
    m_options.auto_reconnect = msg.auto_reconnect;
    m_options.reconnect_delay_ms = msg.reconnect_delay_ms;

    // Re-apply to a live connection
    if (m_transport) {
        m_transport->configure(m_options);
        report_transport_events();
    }
}

void zeromq_relay_actor::process_next_message() {
    // First, poll for inbound messages
    if (m_connected && m_transport) {
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
    m_inbound.clear();
    m_transport->receive(m_inbound, k_max_recv_per_poll);
    report_transport_events();
    for (auto& message : m_inbound) {
        route_message(message);
    }
    m_inbound.clear();
}

void zeromq_relay_actor::report_transport_events() {
    m_events.clear();
    m_transport->drain_events(m_events);
    for (const auto& event : m_events) {
        send_error(event.detail, event.code);
    }
    m_events.clear();
}

void zeromq_relay_actor::route_message(detail::envelope& message) {
    // Create received message
    msg::zeromq_recv recv_msg;
    recv_msg.source = std::move(message.peer);
    recv_msg.correlation_id = message.correlation_id;
    recv_msg.data = std::move(message.payload);
    recv_msg.content_type = std::move(message.content_type);
    recv_msg.message_type = std::move(message.message_type);

    // Check if this is a reply to a pending request
    if (recv_msg.correlation_id != 0) {
        auto it = m_pending_requests.find(recv_msg.correlation_id);
        if (it != m_pending_requests.end()) {
            // Send to original sender
            it->second.requester.tell(recv_msg);
            m_pending_requests.erase(it);
            return;
        }
    }

    // Route by message_type
    if (!recv_msg.message_type.empty()) {
        auto it = m_routes.find(recv_msg.message_type);
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
    std::cout << "[ZEROMQ] No route for message_type '" << recv_msg.message_type << "'\n" << std::flush;
#endif
}

void zeromq_relay_actor::cleanup_socket() {
    if (m_poll_timer != INVALID_TIMER_ID) {
        cancel_timer(m_poll_timer);
        m_poll_timer = INVALID_TIMER_ID;
    }

    m_transport.reset();  // closes; the last one terminates the shared context

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
