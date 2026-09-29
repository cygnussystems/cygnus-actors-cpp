#include "../test_common.h"

#ifdef CAS_ENABLE_ZEROMQ
#include "cas/cas.h"
#include "cas/detail/zeromq_transport.h"
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

    // Relays start on their own threads: wait until both have run on_start()
    using namespace relay_e2e;
    REQUIRE(wait_for([&] { return cas::actor_registry::exists("zeromq_relay"); }));
    std::this_thread::sleep_for(50ms);  // give the second one time to (wrongly) take it

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

// ---------------------------------------------------------------------------
// 070 stage 1, item 2: the relay runs on its own thread and blocks on its
// socket, so latency is not tied to a poll interval
// ---------------------------------------------------------------------------

namespace relay_thread {

struct kick : public cas::message_base {};

// Server side: echoes every request back to its sender, same correlation id
class echo : public cas::actor {
public:
    cas::actor_ref relay;
protected:
    void on_start() override {
        handler<cas::msg::zeromq_recv>([this](const cas::msg::zeromq_recv& in) {
            cas::msg::zeromq_send out;
            out.target = in.source;
            out.correlation_id = in.correlation_id;
            out.message_type = "pong";
            out.data = in.data;
            relay.tell(out);
        });
    }
};

// Client side: sends the next request when the previous reply arrives
class pinger : public cas::actor {
public:
    cas::actor_ref relay;
    int rounds = 0;
    std::atomic<int> completed{0};
protected:
    void on_start() override {
        handler<kick>([this](const kick&) { send_next(); });
        handler<cas::msg::zeromq_recv>([this](const cas::msg::zeromq_recv&) {
            if (completed.fetch_add(1) + 1 < rounds) {
                send_next();
            }
        });
    }
    void send_next() {
        cas::msg::zeromq_send out;
        out.correlation_id = static_cast<uint64_t>(completed.load() + 1);
        out.message_type = "ping";
        out.data = {'p'};
        relay.tell(out);
    }
};

// Sets up server + client relays over inproc with the given relay config,
// runs `rounds` sequential request/reply round trips; returns the elapsed time
std::chrono::milliseconds round_trips(int rounds, const cas::msg::zeromq_config& config,
                                      const std::string& endpoint) {
    auto server = cas::system::create<cas::zeromq_relay_actor>();
    auto client = cas::system::create<cas::zeromq_relay_actor>();
    auto e = cas::system::create<echo>();
    auto p = cas::system::create<pinger>();
    e.get_checked<echo>().relay = server;
    auto& ping = p.get_checked<pinger>();
    ping.relay = client;
    ping.rounds = rounds;
    cas::system::start();

    server.tell(config);
    client.tell(config);

    cas::msg::zeromq_route route;
    route.message_type = "ping";
    route.target = e;
    server.tell(route);

    cas::msg::zeromq_bind bind;
    bind.endpoint = endpoint;
    server.tell(bind);
    std::this_thread::sleep_for(50ms);

    cas::msg::zeromq_connect connect;
    connect.endpoint = endpoint;
    connect.identity = "pinger";
    client.tell(connect);
    std::this_thread::sleep_for(50ms);

    const auto t0 = std::chrono::steady_clock::now();
    p.tell(kick{});
    relay_e2e::wait_for([&] { return ping.completed.load() == rounds; }, 20000);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);
    REQUIRE(ping.completed.load() == rounds);
    return elapsed;
}

} // namespace relay_thread

TEST_CASE("Relay latency is not tied to the poll interval",
          "[08_zeromq_relay][relay_thread]") {
    CAS_TEST_GUARD();
    // With the old poll timer each round trip waited up to two ticks
    // (5 s here); the relay now blocks on its socket and wakes at once.
    cas::msg::zeromq_config config;
    config.poll_interval_ms = 5000;
    const auto elapsed = relay_thread::round_trips(200, config, "inproc://relay-thread-block");
    REQUIRE(elapsed < std::chrono::seconds(5));  // typically a few ms in total
}

TEST_CASE("Relay spin modes deliver too",
          "[08_zeromq_relay][relay_thread]") {
    CAS_TEST_GUARD();
    cas::msg::zeromq_config config;
    SECTION("spin before blocking") {
        config.spin_before_block_us = 200;
        REQUIRE(relay_thread::round_trips(200, config, "inproc://relay-thread-spin") < std::chrono::seconds(5));
    }
    SECTION("never block (busy-poll)") {
        config.spin_before_block_us = -1;
        REQUIRE(relay_thread::round_trips(200, config, "inproc://relay-thread-busy") < std::chrono::seconds(5));
    }
}

TEST_CASE("stop_actor() on an idle relay returns promptly",
          "[08_zeromq_relay][relay_thread]") {
    CAS_TEST_GUARD();
    auto relay = cas::system::create<cas::zeromq_relay_actor>();
    cas::system::start();

    cas::msg::zeromq_bind bind;
    bind.endpoint = "tcp://127.0.0.1:*";
    relay.tell(bind);
    std::this_thread::sleep_for(100ms);  // idle, blocked in the transport

    const auto t0 = std::chrono::steady_clock::now();
    REQUIRE(cas::system::stop_actor(relay));
    // Woken by stop_actor(), not by the 100 ms idle backstop
    REQUIRE(std::chrono::steady_clock::now() - t0 < 80ms);
    REQUIRE_FALSE(relay.is_running());
}

TEST_CASE("System shutdown with an idle relay completes promptly",
          "[08_zeromq_relay][relay_thread]") {
    CAS_TEST_GUARD();
    auto relay = cas::system::create<cas::zeromq_relay_actor>();
    cas::system::start();
    std::this_thread::sleep_for(100ms);  // idle, blocked (not even bound)

    const auto t0 = std::chrono::steady_clock::now();
    cas::system::shutdown();
    cas::system::wait_for_shutdown();
    REQUIRE(std::chrono::steady_clock::now() - t0 < 80ms);
}

// ---------------------------------------------------------------------------
// 070 stage 1, item 3: sends never block the relay; a slow peer is handled
// by zeromq_config::send_policy
// ---------------------------------------------------------------------------

namespace relay_backpressure {

struct send_batch : public cas::message_base {
    int count = 0;
};
struct ask_status : public cas::message_base {};

// Sends numbered messages through the relay; records errors and status
class producer : public cas::actor {
public:
    cas::actor_ref relay;
    std::mutex mutex;
    std::vector<std::string> errors;
    std::atomic<int> statuses{0};
    std::atomic<size_t> queued{0};
    std::atomic<size_t> dropped{0};

    size_t error_count(const std::string& containing) {
        std::lock_guard<std::mutex> lock(mutex);
        size_t n = 0;
        for (const auto& e : errors) n += (e.find(containing) != std::string::npos) ? 1 : 0;
        return n;
    }

protected:
    void on_start() override {
        handler<send_batch>([this](const send_batch& msg) {
            for (int i = 0; i < msg.count; ++i) {
                cas::msg::zeromq_send out;
                out.message_type = "tick";
                const std::string text = std::to_string(i);
                out.data.assign(text.begin(), text.end());
                relay.tell(out);
            }
        });
        handler<ask_status>([this](const ask_status&) { relay.tell(cas::msg::zeromq_status{}); });
        handler<cas::msg::zeromq_error>([this](const cas::msg::zeromq_error& e) {
            std::lock_guard<std::mutex> lock(mutex);
            errors.push_back(e.message);
        });
        handler<cas::msg::zeromq_status>([this](const cas::msg::zeromq_status& s) {
            queued = s.queued_sends;
            dropped = s.dropped_sends;
            statuses.fetch_add(1);
        });
    }
};

// A peer that reads only when told to: a bare transport, not a relay
struct stalled_peer {
    cas::detail::zeromq_transport transport;

    explicit stalled_peer(const std::string& endpoint) {
        cas::detail::transport_options small;
        small.recv_hwm = 1;
        transport.configure(small);
        transport.bind(endpoint);
    }

    std::vector<cas::detail::envelope> read(size_t count, int timeout_ms = 3000) {
        std::vector<cas::detail::envelope> got;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (got.size() < count && std::chrono::steady_clock::now() < deadline) {
            if (transport.receive(got, count - got.size()) == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        return got;
    }
};

struct setup {
    cas::actor_ref relay;
    cas::actor_ref producer_ref;
    producer* p = nullptr;

    setup(const std::string& endpoint, cas::msg::zeromq_config config) {
        relay = cas::system::create<cas::zeromq_relay_actor>();
        producer_ref = cas::system::create<producer>();
        p = &producer_ref.get_checked<producer>();
        p->relay = relay;
        cas::system::start();

        config.send_hwm = 1;  // stall after a couple of messages
        relay.tell(config);
        cas::msg::zeromq_connect connect;
        connect.endpoint = endpoint;
        connect.identity = "producer";
        relay.tell(connect);
        std::this_thread::sleep_for(50ms);
    }

    // Asks the relay for its status and waits for the answer
    void refresh_status() {
        const int before = p->statuses.load();
        producer_ref.tell(ask_status{});
        relay_e2e::wait_for([&] { return p->statuses.load() > before; });
    }
};

} // namespace relay_backpressure

TEST_CASE("Send policy queue: held for a slow peer, delivered in order when it catches up",
          "[08_zeromq_relay][backpressure]") {
    CAS_TEST_GUARD();
    using namespace relay_backpressure;
    stalled_peer peer("inproc://bp-queue");
    cas::msg::zeromq_config config;
    config.send_policy = cas::msg::zeromq_send_policy::queue;
    config.send_timeout_ms = 10000;
    setup s("inproc://bp-queue", config);

    send_batch batch;
    batch.count = 50;
    s.producer_ref.tell(batch);
    REQUIRE(relay_e2e::wait_for([&] { s.refresh_status(); return s.p->queued.load() > 0; }));

    // The relay is not blocked while it holds them: it still answers
    s.refresh_status();
    REQUIRE(s.p->statuses.load() >= 2);

    auto got = peer.read(50);
    REQUIRE(got.size() == 50);
    for (size_t i = 0; i < got.size(); ++i) {
        REQUIRE(std::string(got[i].payload.begin(), got[i].payload.end()) == std::to_string(i));
    }
    REQUIRE(s.p->error_count("") == 0);
    REQUIRE(relay_e2e::wait_for([&] { s.refresh_status(); return s.p->queued.load() == 0; }));
}

TEST_CASE("Send policy queue: a message that waits too long is reported to its sender",
          "[08_zeromq_relay][backpressure]") {
    CAS_TEST_GUARD();
    using namespace relay_backpressure;
    stalled_peer peer("inproc://bp-timeout");
    cas::msg::zeromq_config config;
    config.send_policy = cas::msg::zeromq_send_policy::queue;
    config.send_timeout_ms = 100;
    setup s("inproc://bp-timeout", config);

    send_batch batch;
    batch.count = 20;
    s.producer_ref.tell(batch);
    REQUIRE(relay_e2e::wait_for([&] { return s.p->error_count("timed out") > 0; }));
    REQUIRE(relay_e2e::wait_for([&] { s.refresh_status(); return s.p->queued.load() == 0; }));

    // Every message was either delivered or reported - none silently lost
    auto got = peer.read(20, 500);
    REQUIRE(got.size() + s.p->error_count("timed out") == 20);
}

TEST_CASE("Send policy queue: a full queue is reported at once",
          "[08_zeromq_relay][backpressure]") {
    CAS_TEST_GUARD();
    using namespace relay_backpressure;
    stalled_peer peer("inproc://bp-full");
    cas::msg::zeromq_config config;
    config.send_policy = cas::msg::zeromq_send_policy::queue;
    config.send_timeout_ms = 10000;
    config.max_queued_sends = 3;
    setup s("inproc://bp-full", config);

    send_batch batch;
    batch.count = 20;
    s.producer_ref.tell(batch);
    REQUIRE(relay_e2e::wait_for([&] { return s.p->error_count("queue full") > 0; }));
    s.refresh_status();
    REQUIRE(s.p->queued.load() == 3);
}

TEST_CASE("Send policy fail: reported to the sender at once",
          "[08_zeromq_relay][backpressure]") {
    CAS_TEST_GUARD();
    using namespace relay_backpressure;
    stalled_peer peer("inproc://bp-fail");
    cas::msg::zeromq_config config;
    config.send_policy = cas::msg::zeromq_send_policy::fail;
    setup s("inproc://bp-fail", config);

    send_batch batch;
    batch.count = 20;
    s.producer_ref.tell(batch);
    REQUIRE(relay_e2e::wait_for([&] { return s.p->error_count("not keeping up") > 0; }));
    s.refresh_status();
    REQUIRE(s.p->queued.load() == 0);

    auto got = peer.read(20, 500);
    REQUIRE(got.size() + s.p->error_count("not keeping up") == 20);
}

TEST_CASE("Send policy drop: dropped and counted, no errors",
          "[08_zeromq_relay][backpressure]") {
    CAS_TEST_GUARD();
    using namespace relay_backpressure;
    stalled_peer peer("inproc://bp-drop");
    cas::msg::zeromq_config config;
    config.send_policy = cas::msg::zeromq_send_policy::drop;
    setup s("inproc://bp-drop", config);

    send_batch batch;
    batch.count = 20;
    s.producer_ref.tell(batch);
    REQUIRE(relay_e2e::wait_for([&] { s.refresh_status(); return s.p->dropped.load() > 0; }));
    REQUIRE(s.p->error_count("") == 0);

    auto got = peer.read(20, 500);
    REQUIRE(got.size() + s.p->dropped.load() == 20);
}

// An inline actor as a route target handles the message on the relay's own
// thread; its exception must be reported, not unwound out of that thread
// (INVARIANT 7).
namespace relay_inline_route {

class throwing_inline : public cas::inline_actor<> {
protected:
    void on_start() override {
        handler<cas::msg::zeromq_recv>([](const cas::msg::zeromq_recv&) {
            throw std::runtime_error("inline route target");
        });
    }
};

class counter : public cas::actor {
public:
    std::atomic<int> received{0};
protected:
    void on_start() override {
        handler<cas::msg::zeromq_recv>([this](const cas::msg::zeromq_recv&) { received++; });
    }
};

} // namespace relay_inline_route

TEST_CASE("A throwing inline route target is reported and the relay keeps routing",
          "[08_zeromq_relay][invariant7]") {
    CAS_CONFIG_GUARD();
    using namespace relay_inline_route;
    std::atomic<int> reported{0};
    cas::system::set_handler_exception_handler([&](const cas::handler_exception_info&) { reported++; });

    auto server = cas::system::create<cas::zeromq_relay_actor>();
    auto client = cas::system::create<cas::zeromq_relay_actor>();
    auto bad = cas::system::create<throwing_inline>();
    auto good = cas::system::create<counter>();
    cas::system::start();

    cas::msg::zeromq_route to_bad;
    to_bad.message_type = "bad";
    to_bad.target = bad;
    server.tell(to_bad);
    cas::msg::zeromq_route to_good;
    to_good.message_type = "good";
    to_good.target = good;
    server.tell(to_good);

    cas::msg::zeromq_bind bind;
    bind.endpoint = "inproc://relay-inline-route";
    server.tell(bind);
    std::this_thread::sleep_for(50ms);
    cas::msg::zeromq_connect connect;
    connect.endpoint = "inproc://relay-inline-route";
    connect.identity = "c";
    client.tell(connect);
    std::this_thread::sleep_for(50ms);

    cas::msg::zeromq_send first;
    first.message_type = "bad";
    client.tell(first);
    cas::msg::zeromq_send second;
    second.message_type = "good";
    client.tell(second);

    REQUIRE(relay_e2e::wait_for([&] { return good.get_checked<counter>().received.load() == 1; }));
    REQUIRE(reported.load() == 1);
    REQUIRE(server.is_running());
}

#else

// ZeroMQ not enabled - provide a placeholder test
TEST_CASE("ZeroMQ relay not available", "[08_zeromq_relay][disabled]") {
    // This test runs when CAS_ENABLE_ZEROMQ is not defined
    SUCCEED("ZeroMQ support not enabled");
}

#endif // CAS_ENABLE_ZEROMQ
