#include "../test_common.h"

#ifdef CAS_ENABLE_ZEROMQ
#include "cas/cas.h"
#include <thread>
#include <chrono>
#include <atomic>

using namespace std::chrono_literals;

namespace {

// Test actor that receives ZeroMQ messages
class test_receiver : public cas::actor {
public:
    std::vector<cas::msg::zeromq_recv> received_messages;
    std::vector<cas::msg::zeromq_error> errors;

protected:
    void on_start() override {
        set_name("test_receiver");
        handler<cas::msg::zeromq_recv>([this](const auto& msg) {
            received_messages.push_back(msg);
        });
        handler<cas::msg::zeromq_error>([this](const auto& msg) {
            errors.push_back(msg);
        });
    }
};

} // anonymous namespace

TEST_CASE("ZeroMQ relay actor creation", "[08_zeromq_relay][basic]") {
    cas::system::reset();
    cas::system::configure(cas::system_config{});

    SECTION("Can create relay actor") {
        auto relay = cas::system::create<cas::zeromq_relay_actor>();
        REQUIRE(relay.is_valid());

        cas::system::shutdown();
        cas::system::wait_for_shutdown();
    }
}

TEST_CASE("ZeroMQ bind and connect", "[08_zeromq_relay][connection]") {
    CAS_TEST_GUARD();
    cas::system::reset();
    cas::system::configure(cas::system_config{});

    auto relay = cas::system::create<cas::zeromq_relay_actor>();
    cas::system::start();

    std::this_thread::sleep_for(50ms);

    SECTION("Can bind ROUTER socket") {
        // Request bind
        cas::msg::zeromq_bind bind_msg;
        bind_msg.endpoint = "inproc://test_router";
        relay.tell(bind_msg);

        std::this_thread::sleep_for(50ms);

        // Relay should be running
        REQUIRE(relay.is_running());
    }

    cas::system::shutdown();
    cas::system::wait_for_shutdown();
}

TEST_CASE("ZeroMQ connect", "[08_zeromq_relay][connection]") {
    CAS_TEST_GUARD();
    cas::system::reset();
    cas::system::configure(cas::system_config{});

    auto relay = cas::system::create<cas::zeromq_relay_actor>();
    cas::system::start();

    std::this_thread::sleep_for(50ms);

    SECTION("Can connect DEALER socket") {
        // Request connect
        cas::msg::zeromq_connect connect_msg;
        connect_msg.endpoint = "inproc://test_dealer";
        connect_msg.identity = "test_client";
        relay.tell(connect_msg);

        std::this_thread::sleep_for(50ms);

        REQUIRE(relay.is_running());
    }

    cas::system::shutdown();
    cas::system::wait_for_shutdown();
}

TEST_CASE("ZeroMQ routing setup", "[08_zeromq_relay][routing]") {
    CAS_TEST_GUARD();
    cas::system::reset();
    cas::system::configure(cas::system_config{});

    auto relay = cas::system::create<cas::zeromq_relay_actor>();
    auto receiver1 = cas::system::create<test_receiver>();
    auto receiver2 = cas::system::create<test_receiver>();

    cas::system::start();
    std::this_thread::sleep_for(50ms);

    // Bind
    cas::msg::zeromq_bind bind_msg;
    bind_msg.endpoint = "inproc://test_route";
    relay.tell(bind_msg);

    // Set up routes
    cas::msg::zeromq_route route1;
    route1.message_type = "tick";
    route1.target = receiver1;
    relay.tell(route1);

    cas::msg::zeromq_route route2;
    route2.message_type = "order";
    route2.target = receiver2;
    relay.tell(route2);

    std::this_thread::sleep_for(50ms);

    // Verify relay is running with routes
    REQUIRE(relay.is_running());

    cas::system::shutdown();
    cas::system::wait_for_shutdown();
}

TEST_CASE("ZeroMQ configuration", "[08_zeromq_relay][config]") {
    CAS_TEST_GUARD();
    cas::system::reset();
    cas::system::configure(cas::system_config{});

    auto relay = cas::system::create<cas::zeromq_relay_actor>();
    cas::system::start();

    std::this_thread::sleep_for(50ms);

    // Configure
    cas::msg::zeromq_config config;
    config.poll_interval_ms = 5;
    config.send_timeout_ms = 500;
    config.recv_timeout_ms = 500;
    relay.tell(config);

    // Bind to test configuration
    cas::msg::zeromq_bind bind_msg;
    bind_msg.endpoint = "inproc://test_config";
    relay.tell(bind_msg);

    std::this_thread::sleep_for(50ms);

    REQUIRE(relay.is_running());

    cas::system::shutdown();
    cas::system::wait_for_shutdown();
}

// ---------------------------------------------------------------------------
// Issue 012: end-to-end relay behaviour
// ---------------------------------------------------------------------------

namespace relay_e2e {

struct go : public cas::message_base {};
struct send_request : public cas::message_base {};
struct ask_status : public cas::message_base {};

// Counts deliveries; safe to read from the test thread
class counting_receiver : public cas::actor {
public:
    std::atomic<int> received{0};
protected:
    void on_start() override {
        handler<cas::msg::zeromq_recv>([this](const cas::msg::zeromq_recv&) { received++; });
    }
};

// Sends control messages from a handler, so it is their sender
class controller : public cas::actor {
public:
    cas::actor_ref relay;
    std::string endpoint;
    std::atomic<int> errors{0};
    std::atomic<int> statuses{0};
    std::atomic<long long> last_pending{-1};
protected:
    void on_start() override {
        handler<go>([this](const go&) {
            cas::msg::zeromq_bind bind;
            bind.endpoint = endpoint;
            relay.tell(bind);
        });
        handler<send_request>([this](const send_request&) {
            cas::msg::zeromq_send send;
            send.correlation_id = 77;
            send.data = {'x'};
            relay.tell(send);
        });
        handler<ask_status>([this](const ask_status&) {
            relay.tell(cas::msg::zeromq_status{});
        });
        handler<cas::msg::zeromq_error>([this](const cas::msg::zeromq_error&) { errors++; });
        handler<cas::msg::zeromq_status>([this](const cas::msg::zeromq_status& s) {
            last_pending = static_cast<long long>(s.pending_count);
            statuses++;
        });
    }
};

template<typename Pred>
bool wait_for(Pred pred, int timeout_ms = 3000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!pred() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(2ms);
    }
    return pred();
}

} // namespace relay_e2e

TEST_CASE("Two relays in one process talk over inproc and drain bursts",
          "[08_zeromq_relay][e2e][issue012]") {
    CAS_TEST_GUARD();
    using namespace relay_e2e;

    auto server = cas::system::create<cas::zeromq_relay_actor>();
    auto client = cas::system::create<cas::zeromq_relay_actor>();
    auto sink = cas::system::create<counting_receiver>();
    cas::system::start();

    cas::msg::zeromq_route route;
    route.message_type = "tick";
    route.target = sink;
    server.tell(route);

    cas::msg::zeromq_bind bind;
    bind.endpoint = "inproc://issue012_burst";
    server.tell(bind);
    std::this_thread::sleep_for(50ms);

    cas::msg::zeromq_connect connect;
    connect.endpoint = "inproc://issue012_burst";
    connect.identity = "client";
    client.tell(connect);
    std::this_thread::sleep_for(50ms);

    // At one message per 10 ms poll tick this took ~10 s; drained per tick
    // it takes a few ticks
    constexpr int k_messages = 1000;
    for (int i = 0; i < k_messages; ++i) {
        cas::msg::zeromq_send send;
        send.message_type = "tick";
        send.data = {'a', 'b'};
        client.tell(send);
    }

    auto& s = sink.get_checked<counting_receiver>();
    REQUIRE(wait_for([&] { return s.received.load() == k_messages; }, 3000));
}

TEST_CASE("A failed bind is reported to the sender even with no routes",
          "[08_zeromq_relay][errors][issue012]") {
    CAS_TEST_GUARD();
    using namespace relay_e2e;

    auto relay = cas::system::create<cas::zeromq_relay_actor>();
    auto ctl = cas::system::create<controller>();
    auto& c = ctl.get_checked<controller>();
    c.relay = relay;
    c.endpoint = "notatransport://nowhere";
    cas::system::start();

    ctl.tell(go{});
    REQUIRE(wait_for([&] { return c.errors.load() == 1; }));
}

TEST_CASE("A second relay does not take over the registry name",
          "[08_zeromq_relay][registry][issue012]") {
    CAS_TEST_GUARD();
    auto first = cas::system::create<cas::zeromq_relay_actor>();
    auto second = cas::system::create<cas::zeromq_relay_actor>();
    cas::system::start();

    REQUIRE(cas::actor_registry::get("zeromq_relay") == first);
    REQUIRE(second.name() != "zeromq_relay");
}

TEST_CASE("Unanswered correlation ids expire",
          "[08_zeromq_relay][correlation][issue012]") {
    CAS_TEST_GUARD();
    using namespace relay_e2e;

    auto server = cas::system::create<cas::zeromq_relay_actor>();
    auto client = cas::system::create<cas::zeromq_relay_actor>();
    auto ctl = cas::system::create<controller>();
    auto& c = ctl.get_checked<controller>();
    c.relay = client;
    cas::system::start();

    cas::msg::zeromq_bind bind;
    bind.endpoint = "inproc://issue012_expiry";
    server.tell(bind);

    cas::msg::zeromq_config config;
    config.poll_interval_ms = 5;
    config.request_timeout_ms = 100;
    client.tell(config);
    cas::msg::zeromq_connect connect;
    connect.endpoint = "inproc://issue012_expiry";
    client.tell(connect);
    std::this_thread::sleep_for(50ms);

    ctl.tell(send_request{});   // correlation id 77, never answered
    ctl.tell(ask_status{});
    REQUIRE(wait_for([&] { return c.statuses.load() >= 1; }));
    REQUIRE(c.last_pending.load() == 1);

    std::this_thread::sleep_for(250ms);
    ctl.tell(ask_status{});
    REQUIRE(wait_for([&] { return c.statuses.load() >= 2; }));
    REQUIRE(c.last_pending.load() == 0);
}

#else

// ZeroMQ not enabled - provide a placeholder test
TEST_CASE("ZeroMQ relay not available", "[08_zeromq_relay][disabled]") {
    // This test runs when CAS_ENABLE_ZEROMQ is not defined
    SUCCEED("ZeroMQ support not enabled");
}

#endif // CAS_ENABLE_ZEROMQ
