#include "../test_common.h"

#ifdef CAS_ENABLE_ZEROMQ
#include "cas/cas.h"
#include <thread>
#include <chrono>

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

#else

// ZeroMQ not enabled - provide a placeholder test
TEST_CASE("ZeroMQ relay not available", "[08_zeromq_relay][disabled]") {
    // This test runs when CAS_ENABLE_ZEROMQ is not defined
    SUCCEED("ZeroMQ support not enabled");
}

#endif // CAS_ENABLE_ZEROMQ
