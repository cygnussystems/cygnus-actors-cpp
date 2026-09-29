#ifndef CAS_DETAIL_TRANSPORT_H
#define CAS_DETAIL_TRANSPORT_H

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

// Transport interface behind the relay actor (planning/070, section 10).
//
// A transport moves envelopes - routing metadata plus opaque bytes - between
// processes. It knows nothing about actors: the relay actor owns one and
// turns envelopes into actor messages and back. ZeroMQ is the first
// implementation (detail::zeromq_transport); shared memory, Unix domain
// sockets and others are meant to slot in behind the same interface.
//
// Internal for now (namespace detail): the interface is still settling while
// the ZeroMQ transport is stabilised, and may change within 1.x.
//
// Threading: one owner thread. A transport is used only by the thread that
// executes its relay, so implementations need no locking.

namespace cas::detail {

/// One message on the wire
struct envelope {
    std::string peer;              ///< server: the client it came from / goes to; client: empty
    uint64_t correlation_id = 0;   ///< request/reply matching; 0 = none
    std::string message_type;      ///< routing hint
    std::string content_type;      ///< payload format hint ("fix", "json", "raw", ...)
    std::vector<char> payload;     ///< opaque bytes
};

/// Settings a transport applies to its connection. configure() may be called
/// before or after bind()/connect().
struct transport_options {
    int send_timeout_ms = 1000;       ///< how long a send may block; also the linger on close
    int recv_timeout_ms = 1000;       ///< applied, but receives are non-blocking
    int max_message_size = 1048576;   ///< larger inbound messages are refused by the transport
    bool auto_reconnect = true;       ///< re-establish dropped connections
    int reconnect_delay_ms = 1000;    ///< delay before a reconnect attempt
};

/// Something the transport noticed outside a call's return value
struct transport_event {
    enum class kind {
        error   ///< e.g. a receive error or a malformed inbound message
    };
    kind what = kind::error;
    std::string detail;   ///< human-readable, ready to report
    int code = 0;         ///< transport-specific error code, 0 if none
};

/// A failed bind / connect / send
class transport_error : public std::runtime_error {
public:
    transport_error(const std::string& message, int code);
    int code() const noexcept;

private:
    int m_code;
};

class transport {
public:
    virtual ~transport() = default;

    /// Server role: accept connections on endpoint. Throws transport_error.
    virtual void bind(const std::string& endpoint) = 0;

    /// Client role: connect to endpoint, announcing identity (may be empty).
    /// Throws transport_error.
    virtual void connect(const std::string& endpoint, const std::string& identity) = 0;

    /// Apply options now (if connected) and to any later bind()/connect()
    virtual void configure(const transport_options& options) = 0;

    /// Send one envelope. A server must name the peer. Throws transport_error.
    virtual void send(const envelope& message) = 0;

    /// Non-blocking: append up to max waiting envelopes to out; returns how
    /// many were appended. Problems are reported through drain_events().
    virtual size_t receive(std::vector<envelope>& out, size_t max) = 0;

    /// Move the events recorded since the last call into out
    virtual void drain_events(std::vector<transport_event>& out) = 0;

    /// The endpoint actually in use - e.g. with the port resolved when bound
    /// to a wildcard such as "tcp://127.0.0.1:*". Empty before bind/connect.
    virtual std::string local_endpoint() const = 0;

    /// Release the connection. Idempotent; the destructor also closes.
    virtual void close() = 0;
};

} // namespace cas::detail

#endif // CAS_DETAIL_TRANSPORT_H
