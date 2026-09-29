#ifdef CAS_ENABLE_ZEROMQ

#include "cas/detail/zeromq_transport.h"
#include <zmq.hpp>
#include <cerrno>
#include <mutex>
#include <optional>

namespace cas::detail {

namespace {

// One context for every relay in the process: inproc:// endpoints are only
// visible within a context. Lives as long as some transport holds it.
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

zmq::message_t frame_of(const std::string& text) {
    return zmq::message_t(text.begin(), text.end());
}

std::string text_of(const std::vector<char>& frame) {
    return std::string(frame.begin(), frame.end());
}

} // namespace

struct zeromq_transport::impl {
    std::shared_ptr<zmq::context_t> context;   // declared before socket: destroyed after it
    std::optional<zmq::socket_t> socket;
    bool server = false;                       // ROUTER (bound) vs DEALER (connected)
    transport_options options;
    std::vector<transport_event> events;

    void create_socket(int type) {
        if (!context) {
            context = shared_context();
        }
        socket.emplace(*context, type);
        server = (type == ZMQ_ROUTER);
    }

    void apply_options() {
        if (!socket) return;
        socket->set(zmq::sockopt::sndtimeo, options.send_timeout_ms);
        socket->set(zmq::sockopt::rcvtimeo, options.recv_timeout_ms);
        socket->set(zmq::sockopt::maxmsgsize, static_cast<int64_t>(options.max_message_size));

        // Bounded linger: the default (-1) makes the last transport's context
        // teardown wait forever for undeliverable outbound messages
        socket->set(zmq::sockopt::linger, options.send_timeout_ms);

        // ZeroMQ reconnects dropped connections by itself; -1 disables it
        socket->set(zmq::sockopt::reconnect_ivl, options.auto_reconnect ? options.reconnect_delay_ms : -1);

        // ROUTER: report unknown identities as errors instead of dropping silently
        if (server) {
            socket->set(zmq::sockopt::router_mandatory, true);
        }
    }

    void record_error(const std::string& detail, int code) {
        transport_event event;
        event.what = transport_event::kind::error;
        event.detail = detail;
        event.code = code;
        events.push_back(std::move(event));
    }

    // One multipart message; false when none is waiting (or on error)
    bool receive_one(std::vector<envelope>& out) {
        try {
            zmq::message_t frame;
            std::vector<std::vector<char>> frames;

            auto result = socket->recv(frame, zmq::recv_flags::dontwait);
            if (!result.has_value()) {
                return false;  // nothing waiting
            }
            frames.emplace_back(frame.data<char>(), frame.data<char>() + frame.size());
            while (frame.more()) {
                result = socket->recv(frame, zmq::recv_flags::dontwait);
                if (!result.has_value()) {
                    break;  // incomplete multipart message
                }
                frames.emplace_back(frame.data<char>(), frame.data<char>() + frame.size());
            }

            // ROUTER: [identity][correlation_id][payload][content_type][message_type]
            // DEALER: [correlation_id][payload][content_type][message_type]
            const size_t min_frames = server ? 5 : 4;
            if (frames.size() < min_frames) {
                record_error("Received malformed message (frame count: " +
                             std::to_string(frames.size()) + ")", 0);
                return true;  // consumed; keep reading
            }

            envelope message;
            size_t idx = 0;
            if (server) {
                message.peer = text_of(frames[0]);
                idx = 1;
            }
            try {
                message.correlation_id = std::stoull(text_of(frames[idx]));
            } catch (...) {
                message.correlation_id = 0;
            }
            message.payload = std::move(frames[idx + 1]);
            message.content_type = text_of(frames[idx + 2]);
            message.message_type = text_of(frames[idx + 3]);
            out.push_back(std::move(message));
            return true;

        } catch (const zmq::error_t& e) {
            if (e.num() != EAGAIN) {
                record_error(std::string("Receive error: ") + e.what(), e.num());
            }
            return false;
        }
    }
};

zeromq_transport::zeromq_transport() : m_impl(std::make_unique<impl>()) {}

zeromq_transport::~zeromq_transport() {
    close();
}

void zeromq_transport::bind(const std::string& endpoint) {
    try {
        m_impl->create_socket(ZMQ_ROUTER);
        m_impl->apply_options();
        m_impl->socket->bind(endpoint);
    } catch (const zmq::error_t& e) {
        close();
        throw transport_error(e.what(), e.num());
    }
}

void zeromq_transport::connect(const std::string& endpoint, const std::string& identity) {
    try {
        m_impl->create_socket(ZMQ_DEALER);
        if (!identity.empty()) {
            m_impl->socket->set(zmq::sockopt::routing_id, zmq::buffer(identity));
        }
        m_impl->apply_options();
        m_impl->socket->connect(endpoint);
    } catch (const zmq::error_t& e) {
        close();
        throw transport_error(e.what(), e.num());
    }
}

void zeromq_transport::configure(const transport_options& options) {
    m_impl->options = options;
    try {
        m_impl->apply_options();
    } catch (const zmq::error_t& e) {
        m_impl->record_error(std::string("Configure failed: ") + e.what(), e.num());
    }
}

void zeromq_transport::send(const envelope& message) {
    if (!m_impl->socket) {
        throw transport_error("Not connected", 0);
    }
    if (m_impl->server && message.peer.empty()) {
        throw transport_error("ROUTER mode requires target identity", 0);
    }

    try {
        auto& socket = *m_impl->socket;
        if (m_impl->server) {
            zmq::message_t identity = frame_of(message.peer);
            socket.send(identity, zmq::send_flags::sndmore);
        }
        zmq::message_t correlation = frame_of(std::to_string(message.correlation_id));
        socket.send(correlation, zmq::send_flags::sndmore);

        zmq::message_t payload(message.payload.begin(), message.payload.end());
        socket.send(payload, zmq::send_flags::sndmore);

        zmq::message_t content_type = frame_of(message.content_type);
        socket.send(content_type, zmq::send_flags::sndmore);

        zmq::message_t message_type = frame_of(message.message_type);
        socket.send(message_type, zmq::send_flags::none);
    } catch (const zmq::error_t& e) {
        throw transport_error(e.what(), e.num());
    }
}

size_t zeromq_transport::receive(std::vector<envelope>& out, size_t max) {
    if (!m_impl->socket) {
        return 0;
    }
    const size_t before = out.size();
    for (size_t i = 0; i < max && m_impl->socket; ++i) {
        if (!m_impl->receive_one(out)) {
            break;
        }
    }
    return out.size() - before;
}

void zeromq_transport::drain_events(std::vector<transport_event>& out) {
    for (auto& event : m_impl->events) {
        out.push_back(std::move(event));
    }
    m_impl->events.clear();
}

std::string zeromq_transport::local_endpoint() const {
    if (!m_impl->socket) {
        return std::string();
    }
    return m_impl->socket->get(zmq::sockopt::last_endpoint);
}

void zeromq_transport::close() {
    m_impl->socket.reset();
    m_impl->context.reset();  // the last transport's release terminates the context
}

} // namespace cas::detail

#endif // CAS_ENABLE_ZEROMQ
