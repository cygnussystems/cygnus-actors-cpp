#include "../test_common.h"

// The transport layer on its own, without actors (planning/070, section 10):
// envelopes round-trip between a bound (server) and a connected (client)
// transport, errors surface as transport_error or events.

#ifdef CAS_ENABLE_ZEROMQ

#include "cas/detail/zeromq_transport.h"
#include <zmq.hpp>
#include <chrono>
#include <thread>

namespace transport_test {

using cas::detail::envelope;
using cas::detail::transport_error;
using cas::detail::transport_event;
using cas::detail::zeromq_transport;

// Polls a transport until it yields `count` envelopes or the timeout passes
std::vector<envelope> receive_n(zeromq_transport& t, size_t count, int timeout_ms = 2000) {
    std::vector<envelope> got;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (got.size() < count && std::chrono::steady_clock::now() < deadline) {
        if (t.receive(got, count - got.size()) == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    return got;
}

envelope make(const std::string& type, const std::string& text, uint64_t correlation = 0) {
    envelope e;
    e.message_type = type;
    e.content_type = "text";
    e.correlation_id = correlation;
    e.payload.assign(text.begin(), text.end());
    return e;
}

std::string text(const envelope& e) {
    return std::string(e.payload.begin(), e.payload.end());
}

} // namespace transport_test

using namespace transport_test;

TEST_CASE("Transport: envelopes round-trip between server and client",
          "[08_zeromq_relay][transport]") {
    zeromq_transport server;
    server.bind("tcp://127.0.0.1:*");
    const std::string endpoint = server.local_endpoint();
    REQUIRE(endpoint.rfind("tcp://127.0.0.1:", 0) == 0);
    REQUIRE(endpoint != "tcp://127.0.0.1:*");  // the port was resolved

    zeromq_transport client;
    client.connect(endpoint, "client-7");

    // Client -> server: the server sees who sent it
    REQUIRE(client.try_send(make("NewOrderSingle", "BUY 1", 42)));
    auto at_server = receive_n(server, 1);
    REQUIRE(at_server.size() == 1);
    REQUIRE(at_server[0].peer == "client-7");
    REQUIRE(at_server[0].correlation_id == 42);
    REQUIRE(at_server[0].message_type == "NewOrderSingle");
    REQUIRE(at_server[0].content_type == "text");
    REQUIRE(text(at_server[0]) == "BUY 1");

    // Server -> client: addressed by that peer; the client sees no peer
    envelope reply = make("OrderAck", "ACK", 42);
    reply.peer = at_server[0].peer;
    REQUIRE(server.try_send(reply));
    auto at_client = receive_n(client, 1);
    REQUIRE(at_client.size() == 1);
    REQUIRE(at_client[0].peer.empty());
    REQUIRE(at_client[0].correlation_id == 42);
    REQUIRE(text(at_client[0]) == "ACK");
}

TEST_CASE("Transport: receive drains up to the budget and preserves order",
          "[08_zeromq_relay][transport]") {
    zeromq_transport server;
    server.bind("tcp://127.0.0.1:*");
    zeromq_transport client;
    client.connect(server.local_endpoint(), "c");

    for (int i = 0; i < 50; ++i) {
        REQUIRE(client.try_send(make("tick", std::to_string(i))));
    }
    auto got = receive_n(server, 50);
    REQUIRE(got.size() == 50);
    for (int i = 0; i < 50; ++i) {
        REQUIRE(text(got[static_cast<size_t>(i)]) == std::to_string(i));
    }

    std::vector<envelope> none;
    REQUIRE(server.receive(none, 10) == 0);  // non-blocking when empty
}

TEST_CASE("Transport: bind and send failures throw transport_error",
          "[08_zeromq_relay][transport]") {
    zeromq_transport bad;
    REQUIRE_THROWS_AS(bad.bind("notatransport://x"), transport_error);
    try {
        bad.bind("notatransport://x");
    } catch (const transport_error& e) {
        REQUIRE(e.code() != 0);
    }

    zeromq_transport unconnected;
    REQUIRE_THROWS_AS(unconnected.try_send(make("t", "x")), transport_error);

    zeromq_transport server;
    server.bind("tcp://127.0.0.1:*");
    REQUIRE_THROWS_AS(server.try_send(make("t", "x")), transport_error);  // a server must name the peer
}

TEST_CASE("Transport: a malformed inbound message becomes an error event",
          "[08_zeromq_relay][transport]") {
    zeromq_transport server;
    server.bind("tcp://127.0.0.1:*");

    // A raw peer that does not speak the relay's frame layout
    zmq::context_t context(1);
    zmq::socket_t raw(context, ZMQ_DEALER);
    raw.set(zmq::sockopt::linger, 0);
    raw.connect(server.local_endpoint());
    raw.send(zmq::str_buffer("only"), zmq::send_flags::sndmore);
    raw.send(zmq::str_buffer("two"), zmq::send_flags::none);

    std::vector<transport_event> events;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (events.empty() && std::chrono::steady_clock::now() < deadline) {
        std::vector<envelope> got;
        server.receive(got, 10);
        REQUIRE(got.empty());
        server.drain_events(events);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].detail == "Received malformed message (frame count: 3)");

    events.clear();
    server.drain_events(events);
    REQUIRE(events.empty());  // drained once
}

TEST_CASE("Transport: close is idempotent and ends the connection",
          "[08_zeromq_relay][transport]") {
    zeromq_transport server;
    server.bind("tcp://127.0.0.1:*");
    REQUIRE_FALSE(server.local_endpoint().empty());
    server.close();
    server.close();
    REQUIRE(server.local_endpoint().empty());
    std::vector<envelope> got;
    REQUIRE(server.receive(got, 10) == 0);
}

// --- wait() / wake() (070 stage 1, item 2) ------------------------------------

TEST_CASE("Transport: wait() times out when nothing happens",
          "[08_zeromq_relay][transport][wait]") {
    zeromq_transport t;  // works before bind/connect too
    const auto t0 = std::chrono::steady_clock::now();
    t.wait(std::chrono::milliseconds(50));
    const auto waited = std::chrono::steady_clock::now() - t0;
    REQUIRE(waited >= std::chrono::milliseconds(40));
    REQUIRE(waited < std::chrono::seconds(2));
}

TEST_CASE("Transport: wake() from another thread ends a wait()",
          "[08_zeromq_relay][transport][wait]") {
    zeromq_transport t;
    t.bind("tcp://127.0.0.1:*");

    std::thread waker([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        t.wake();
    });
    const auto t0 = std::chrono::steady_clock::now();
    t.wait(std::chrono::seconds(10));
    const auto waited = std::chrono::steady_clock::now() - t0;
    waker.join();
    REQUIRE(waited < std::chrono::seconds(2));
}

TEST_CASE("Transport: a wake() before wait() makes it return at once, once",
          "[08_zeromq_relay][transport][wait]") {
    zeromq_transport t;
    t.wake();
    t.wake();  // several wake-ups before a wait are answered by one wait
    const auto t0 = std::chrono::steady_clock::now();
    t.wait(std::chrono::seconds(10));
    REQUIRE(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(1));

    // Consumed: the next wait blocks until its timeout
    const auto t1 = std::chrono::steady_clock::now();
    t.wait(std::chrono::milliseconds(50));
    REQUIRE(std::chrono::steady_clock::now() - t1 >= std::chrono::milliseconds(40));
}

TEST_CASE("Transport: inbound data ends a wait()",
          "[08_zeromq_relay][transport][wait]") {
    zeromq_transport server;
    server.bind("tcp://127.0.0.1:*");
    zeromq_transport client;
    client.connect(server.local_endpoint(), "c");

    std::thread sender([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        client.try_send(make("tick", "x"));
    });
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<envelope> got;
    while (got.empty() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5)) {
        server.wait(std::chrono::seconds(10));
        server.receive(got, 10);
    }
    sender.join();
    REQUIRE(got.size() == 1);
    REQUIRE(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));
}

// --- back-pressure: try_send() (070 stage 1, item 3) ----------------------------

TEST_CASE("Transport: try_send() refuses when the peer is not reading, then recovers",
          "[08_zeromq_relay][transport][backpressure]") {
    cas::detail::transport_options small;
    small.send_hwm = 1;
    small.recv_hwm = 1;

    zeromq_transport server;  // never reads until told to
    server.configure(small);
    server.bind("inproc://transport-backpressure");

    zeromq_transport client;
    client.configure(small);
    client.connect("inproc://transport-backpressure", "c");

    // inproc: the queue between the two holds send_hwm + recv_hwm messages
    int accepted = 0;
    bool refused = false;
    for (int i = 0; i < 1000 && !refused; ++i) {
        if (client.try_send(make("tick", std::to_string(i)))) {
            ++accepted;
        } else {
            refused = true;
        }
    }
    REQUIRE(refused);
    REQUIRE(accepted >= 1);
    REQUIRE(accepted < 1000);

    // Nothing half-sent: the server gets exactly the accepted messages, whole
    auto got = receive_n(server, static_cast<size_t>(accepted));
    REQUIRE(got.size() == static_cast<size_t>(accepted));
    for (int i = 0; i < accepted; ++i) {
        REQUIRE(text(got[static_cast<size_t>(i)]) == std::to_string(i));
    }

    // Once the server has read, the client can send again
    bool resumed = false;
    for (int i = 0; i < 2000 && !resumed; ++i) {
        resumed = client.try_send(make("tick", "again"));
        if (!resumed) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(resumed);
}

#endif // CAS_ENABLE_ZEROMQ
