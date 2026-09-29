#ifndef CAS_DETAIL_ZEROMQ_TRANSPORT_H
#define CAS_DETAIL_ZEROMQ_TRANSPORT_H

#ifdef CAS_ENABLE_ZEROMQ

#include "transport.h"
#include <memory>

namespace cas::detail {

/// ZeroMQ transport: ROUTER when bound (server), DEALER when connected
/// (client). All relays in a process share one ZeroMQ context, so inproc://
/// works between them.
///
/// wait() is a zmq_poll on the data socket and an internal inproc PAIR used
/// by wake(); wake() sends on the other end of that pair under a mutex (a
/// ZeroMQ socket may move between threads given a full barrier).
///
/// Wire format - one multipart message per envelope:
///   client -> server: [correlation_id][payload][content_type][message_type]
///   server -> client: same four frames; the ROUTER prepends / strips the
///                     client identity (envelope::peer) on the server side
/// correlation_id travels as decimal text.
class zeromq_transport : public transport {
public:
    zeromq_transport();
    ~zeromq_transport() override;

    zeromq_transport(const zeromq_transport&) = delete;
    zeromq_transport& operator=(const zeromq_transport&) = delete;

    void bind(const std::string& endpoint) override;
    void connect(const std::string& endpoint, const std::string& identity) override;
    void configure(const transport_options& options) override;
    void send(const envelope& message) override;
    size_t receive(std::vector<envelope>& out, size_t max) override;
    void drain_events(std::vector<transport_event>& out) override;
    void wait(std::chrono::milliseconds timeout) override;
    void wake() override;
    std::string local_endpoint() const override;
    void close() override;

private:
    struct impl;                   // keeps zmq.hpp out of this header
    std::unique_ptr<impl> m_impl;
};

} // namespace cas::detail

#endif // CAS_ENABLE_ZEROMQ

#endif // CAS_DETAIL_ZEROMQ_TRANSPORT_H
