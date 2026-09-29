// Tutorial 17: Talking to other processes - the ZeroMQ relay
//
// TEACHES
//   - zeromq_relay_actor as a server (ROUTER, zeromq_bind) and a client
//     (DEALER, zeromq_connect)
//   - routing inbound messages to actors by message_type, with a default route
//   - request/reply: a correlation id brings the reply back to the requester
//   - replying to one client (ROUTER target = the sender's identity)
//   - pushing unsolicited messages (market data) to a client
//   - errors: sent to the actor whose control message failed
//   - asking a relay for its status
//
// Each relay runs on its own thread and blocks on its socket when idle, so
// messages are handled as they arrive (see zeromq_config::spin_before_block_us
// for trading the CPU of a spinning thread against a few µs of wake-up).
//
// ASSUMES
//   Tutorials 1-3. Only built when the framework is built with ZeroMQ
//   (CMake ENABLE_ZEROMQ=ON, vcpkg cppzmq).
//
// THE IDEA
//   Inside a process, actors talk with tell(). Between processes, a relay
//   actor turns messages into ZeroMQ frames and back. Payloads are opaque
//   bytes - FIX, JSON, protobuf, whatever both sides agree on - plus two
//   routing hints: content_type and message_type.
//
//   Here both sides run in one process and talk over inproc:// so the
//   tutorial is self-contained. Between processes you would use
//   "tcp://*:5555" / "tcp://host:5555" or "ipc://..." instead; nothing else
//   changes.

#include "tutorials.h"
#include "cas/cas.h"

#include <atomic>
#include <iostream>
#include <mutex>
#include <string>

namespace {

using cas::msg::zeromq_bind;
using cas::msg::zeromq_connect;
using cas::msg::zeromq_error;
using cas::msg::zeromq_recv;
using cas::msg::zeromq_route;
using cas::msg::zeromq_send;
using cas::msg::zeromq_status;

constexpr const char* k_endpoint = "inproc://tutorial-exchange";

std::vector<char> bytes(const std::string& text) {
    return {text.begin(), text.end()};
}

std::string text(const std::vector<char>& data) {
    return {data.begin(), data.end()};
}

// Kick-off messages from main()
struct setup : cas::message_base {
    cas::actor_ref relay;
};
struct place_order : cas::message_base {};
struct push_market_data : cas::message_base {};
struct bind_again : cas::message_base {};

// --- What main() observes ------------------------------------------------------

std::atomic<bool> g_server_bound{false};
std::atomic<bool> g_client_connected{false};
std::atomic<int> g_orders_received{0};   // at the exchange
std::atomic<int> g_acks_received{0};     // at the trader, via correlation id
std::atomic<int> g_market_data{0};       // at the trader, via its default route
std::atomic<int> g_errors{0};            // at the exchange
std::atomic<long long> g_client_pending{-1};
std::mutex g_mutex;
std::string g_last_ack;                  // guarded by g_mutex

// --- Server side: the exchange ------------------------------------------------------
//
// Owns the server relay. Every control message is sent from a handler, so the
// exchange is their sender: status replies and errors come back here.

class exchange : public cas::actor {
protected:
    void on_start() override {
        set_name("exchange");
        handler<setup>(&exchange::on_setup);
        handler<zeromq_recv>(&exchange::on_recv);
        handler<zeromq_status>(&exchange::on_status);
        handler<zeromq_error>(&exchange::on_error);
        handler<push_market_data>(&exchange::on_push);
        handler<bind_again>(&exchange::on_bind_again);
    }

    void on_setup(const setup& msg) {
        m_relay = msg.relay;

        // Route first, then bind: messages arriving right after the bind
        // already have somewhere to go. Inbound frames whose message_type is
        // "NewOrderSingle" come to this actor.
        zeromq_route route;
        route.message_type = "NewOrderSingle";
        route.target = self();
        m_relay.tell(route);

        zeromq_bind bind;
        bind.endpoint = k_endpoint;    // e.g. "tcp://*:5555" between processes
        m_relay.tell(bind);
    }

    // After a successful bind the relay reports its status to the sender
    void on_status(const zeromq_status& msg) {
        if (msg.connected && msg.mode == "router") {
            std::cout << "  exchange: listening on " << msg.endpoint << "\n";
            g_server_bound = true;
        }
    }

    void on_recv(const zeromq_recv& msg) {
        std::cout << "  exchange: " << msg.message_type << " from '" << msg.source
                  << "': " << text(msg.data) << "\n";
        g_orders_received.fetch_add(1);
        m_client = msg.source;  // the client's identity, for pushes later

        // Reply to that one client: a ROUTER needs the target identity. The
        // correlation id is echoed so the client relay can hand the reply to
        // whoever sent the request.
        zeromq_send ack;
        ack.target = msg.source;
        ack.correlation_id = msg.correlation_id;
        ack.content_type = "text";
        ack.message_type = "OrderAck";
        ack.data = bytes("ACK " + text(msg.data));
        m_relay.tell(ack);
    }

    // Unsolicited: no correlation id, so the client routes it by type
    void on_push(const push_market_data&) {
        zeromq_send md;
        md.target = m_client;
        md.content_type = "text";
        md.message_type = "MarketData";
        md.data = bytes("ES 5321.25");
        m_relay.tell(md);
    }

    // Binding twice is an error - reported to us, the sender
    void on_bind_again(const bind_again&) {
        zeromq_bind bind;
        bind.endpoint = k_endpoint;
        m_relay.tell(bind);
    }

    void on_error(const zeromq_error& msg) {
        std::cout << "  exchange: relay error: " << msg.message << "\n";
        g_errors.fetch_add(1);
    }

private:
    cas::actor_ref m_relay;
    std::string m_client;
};

// --- Client side: the trader ---------------------------------------------------------

class trader : public cas::actor {
protected:
    void on_start() override {
        set_name("trader");
        handler<setup>(&trader::on_setup);
        handler<place_order>(&trader::on_place_order);
        handler<zeromq_recv>(&trader::on_recv);
        handler<zeromq_status>(&trader::on_status);
    }

    void on_setup(const setup& msg) {
        m_relay = msg.relay;

        // Default route (empty message_type): everything that is neither a
        // reply to one of our requests nor routed by type comes here
        zeromq_route route;
        route.target = self();
        m_relay.tell(route);

        zeromq_connect connect;
        connect.endpoint = k_endpoint;   // e.g. "tcp://exchange-host:5555"
        connect.identity = "trader-1";   // how the exchange addresses us
        m_relay.tell(connect);
    }

    void on_place_order(const place_order&) {
        // Request: a non-zero correlation id, sent from inside this actor,
        // makes the client relay remember us as the requester. The reply
        // comes back to this actor whatever routes are set.
        zeromq_send order;
        order.correlation_id = 1;
        order.content_type = "text";
        order.message_type = "NewOrderSingle";
        order.data = bytes("BUY 2 ES @ 5321.00");
        m_relay.tell(order);
    }

    void on_recv(const zeromq_recv& msg) {
        std::cout << "  trader: " << msg.message_type << ": " << text(msg.data) << "\n";
        if (msg.message_type == "OrderAck" && msg.correlation_id == 1) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_last_ack = text(msg.data);
            g_acks_received.fetch_add(1);
        } else if (msg.message_type == "MarketData") {
            g_market_data.fetch_add(1);
        }
    }

    void on_status(const zeromq_status& msg) {
        if (msg.mode == "dealer" && msg.connected) {
            g_client_connected = true;
        }
    }

private:
    cas::actor_ref m_relay;
};

// --- A monitor that asks a relay for its status --------------------------------------

class status_monitor : public cas::actor {
public:
    explicit status_monitor(cas::actor_ref relay) : m_relay(std::move(relay)) {}

protected:
    void on_start() override {
        handler<zeromq_status>(&status_monitor::on_status);
        m_relay.tell(zeromq_status{});  // the reply comes to this actor
    }

    void on_status(const zeromq_status& msg) {
        std::cout << "  monitor: " << msg.mode << " on " << msg.endpoint << ", "
                  << msg.route_count << " typed route(s), " << msg.pending_count
                  << " pending request(s)\n";
        g_client_pending = static_cast<long long>(msg.pending_count);
    }

private:
    cas::actor_ref m_relay;
};

} // namespace

namespace tut {

bool run_zeromq_relay() {
    bool ok = true;

    section("Two relays, an exchange and a trader");
    auto server_relay = cas::system::create<cas::zeromq_relay_actor>();
    auto client_relay = cas::system::create<cas::zeromq_relay_actor>();
    auto exchange_ref = cas::system::create<exchange>();
    auto trader_ref = cas::system::create<trader>();
    cas::system::start();

    // --- 1. Server binds, client connects -----------------------------------------

    section("Binding and connecting");
    setup server_setup;
    server_setup.relay = server_relay;
    exchange_ref.tell(server_setup);
    ok &= check(wait_until([] { return g_server_bound.load(); }), "the exchange's relay is bound");

    setup client_setup;
    client_setup.relay = client_relay;
    trader_ref.tell(client_setup);
    ok &= check(wait_until([] { return g_client_connected.load(); }), "the trader's relay is connected");

    // --- 2. Request and reply ---------------------------------------------------------

    section("The trader sends an order; the exchange acknowledges it");
    trader_ref.tell(place_order{});
    ok &= check(wait_until([] { return g_orders_received.load() == 1; }),
                "the exchange received the order (routed by message_type)");
    ok &= check(wait_until([] { return g_acks_received.load() == 1; }),
                "the ack came back to the trader (matched by correlation id)");
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ok &= check(g_last_ack == "ACK BUY 2 ES @ 5321.00", "with the exchange's payload");
    }

    // --- 3. Unsolicited messages ---------------------------------------------------------

    section("The exchange pushes market data");
    exchange_ref.tell(push_market_data{});
    ok &= check(wait_until([] { return g_market_data.load() == 1; }),
                "the trader got it through its default route");

    // --- 4. Status and errors ------------------------------------------------------------

    section("Asking the client relay for its status");
    // A relay answers a zeromq_status request to its sender, so ask from an
    // actor (from main() there would be no one to reply to)
    g_client_pending = -1;
    cas::system::create<status_monitor>(client_relay);
    ok &= check(wait_until([] { return g_client_pending.load() == 0; }),
                "no request is still waiting for a reply");

    section("Binding twice is reported to the sender");
    exchange_ref.tell(bind_again{});
    ok &= check(wait_until([] { return g_errors.load() == 1; }),
                "the exchange received a zeromq_error");

    section("Shutting down");
    cas::system::shutdown();
    cas::system::wait_for_shutdown();

    return ok;
}

} // namespace tut
