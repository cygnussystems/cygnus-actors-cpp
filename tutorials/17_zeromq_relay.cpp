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
// ASSUMES
//   Tutorials 1-3. Only built when the framework is built with ZeroMQ
//   (CMake ENABLE_ZEROMQ=ON, vcpkg cppzmq).
//
// THE IDEA
//   Inside a process, actors talk with tell(). Between processes, a relay
//   actor turns messages into ZeroMQ frames and back. Payloads are opaque
//   bytes - FIX, JSON, protobuf, whatever both sides agree on - plus two
//   routing hints: content_type and message_type. Each relay runs on its own
//   thread and blocks on its socket when idle (zeromq_config::spin_before_block_us
//   trades a spinning CPU for a few µs of wake-up).
//
//   Both sides run in one process over inproc:// so the tutorial is
//   self-contained. Between processes use "tcp://*:5555" / "tcp://host:5555"
//   or "ipc://..." instead; nothing else changes.

#include "tutorials.h"
#include "cas/cas.h"

#include <atomic>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

namespace {

// Protocol: the relay's control and data messages, used by every actor below.
using cas::msg::zeromq_bind;
using cas::msg::zeromq_connect;
using cas::msg::zeromq_error;
using cas::msg::zeromq_recv;
using cas::msg::zeromq_route;
using cas::msg::zeromq_send;
using cas::msg::zeromq_status;

// Payloads are bytes; these tutorial payloads are text.
std::vector<char> bytes(const std::string& s) { return {s.begin(), s.end()}; }
std::string text(const std::vector<char>& data) { return {data.begin(), data.end()}; }

// Server side. Owns the server relay and sends every control message from a
// handler, so it is their sender: status replies and errors come back here.
class exchange : public cas::actor {
public:
    struct listen : cas::message_base {};            // from main()
    struct push_market_data : cas::message_base {};  // from main()
    struct bind_again : cas::message_base {};        // from main(): provoke an error

    static constexpr const char* endpoint = "inproc://tutorial-exchange";  // e.g. "tcp://*:5555"

    static inline std::atomic<bool> bound{false};  // observed by the tutorial
    static inline std::atomic<int> orders{0};
    static inline std::atomic<int> errors{0};

    explicit exchange(cas::actor_ref relay) : m_relay(std::move(relay)) {}

protected:
    void on_start() override {
        set_name("exchange");
        handler<listen>(&exchange::on_listen);
        handler<push_market_data>(&exchange::on_push);
        handler<bind_again>(&exchange::on_bind_again);
        handler<zeromq_recv>(&exchange::on_recv);
        handler<zeromq_status>(&exchange::on_status);
        handler<zeromq_error>(&exchange::on_error);
    }

private:
    // Route first, then bind: messages arriving right after the bind already
    // have somewhere to go. Inbound "NewOrderSingle" frames come to this actor.
    void on_listen(const listen&) {
        zeromq_route route;
        route.message_type = "NewOrderSingle";
        route.target = self();
        m_relay.tell(route);
        bind();
    }

    // Binding twice is an error - reported to us, the sender
    void on_bind_again(const bind_again&) { bind(); }

    void bind() {
        zeromq_bind msg;
        msg.endpoint = endpoint;
        m_relay.tell(msg);
    }

    // After a successful bind the relay reports its status to the sender
    void on_status(const zeromq_status& msg) {
        if (msg.connected && msg.mode == "router") {
            std::cout << "  exchange: listening on " << msg.endpoint << "\n";
            bound = true;
        }
    }

    // Reply to that one client: a ROUTER needs the target identity. The
    // correlation id is echoed so the client relay can hand the reply to
    // whoever sent the request.
    void on_recv(const zeromq_recv& msg) {
        std::cout << "  exchange: " << msg.message_type << " from '" << msg.source
                  << "': " << text(msg.data) << "\n";
        orders.fetch_add(1);
        m_client = msg.source;  // remembered for pushes later
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

    void on_error(const zeromq_error& msg) {
        std::cout << "  exchange: relay error: " << msg.message << "\n";
        errors.fetch_add(1);
    }

    cas::actor_ref m_relay;
    std::string m_client;
};

// Client side.
class trader : public cas::actor {
public:
    struct connect : cas::message_base {};      // from main()
    struct place_order : cas::message_base {};  // from main()

    static constexpr const char* order_text = "BUY 2 ES @ 5321.00";

    static inline std::atomic<bool> connected{false};  // observed by the tutorial
    static inline std::atomic<int> acks{0};            // matched by correlation id
    static inline std::atomic<int> market_data{0};     // via the default route

    static std::string last_ack() {
        std::lock_guard<std::mutex> lock(s_mutex);
        return s_last_ack;
    }

    explicit trader(cas::actor_ref relay) : m_relay(std::move(relay)) {}

protected:
    void on_start() override {
        set_name("trader");
        handler<connect>(&trader::on_connect);
        handler<place_order>(&trader::on_place_order);
        handler<zeromq_recv>(&trader::on_recv);
        handler<zeromq_status>(&trader::on_status);
    }

private:
    // Default route (empty message_type): everything that is neither a reply
    // to one of our requests nor routed by type comes here
    void on_connect(const connect&) {
        zeromq_route route;
        route.target = self();
        m_relay.tell(route);

        zeromq_connect conn;
        conn.endpoint = exchange::endpoint;  // e.g. "tcp://exchange-host:5555"
        conn.identity = "trader-1";          // how the exchange addresses us
        m_relay.tell(conn);
    }

    void on_status(const zeromq_status& msg) {
        if (msg.mode == "dealer" && msg.connected) connected = true;
    }

    // Request: a non-zero correlation id, sent from inside this actor, makes
    // the client relay remember us as the requester; the reply comes back
    // here whatever routes are set.
    void on_place_order(const place_order&) {
        zeromq_send order;
        order.correlation_id = 1;
        order.content_type = "text";
        order.message_type = "NewOrderSingle";
        order.data = bytes(order_text);
        m_relay.tell(order);
    }

    void on_recv(const zeromq_recv& msg) {
        std::cout << "  trader: " << msg.message_type << ": " << text(msg.data) << "\n";
        if (msg.message_type == "OrderAck" && msg.correlation_id == 1) {
            {
                std::lock_guard<std::mutex> lock(s_mutex);
                s_last_ack = text(msg.data);
            }
            acks.fetch_add(1);
        } else if (msg.message_type == "MarketData") {
            market_data.fetch_add(1);
        }
    }

    cas::actor_ref m_relay;

    static inline std::mutex s_mutex;
    static inline std::string s_last_ack;
};

// Asks a relay for its status. A relay answers to the sender, so the question
// must come from an actor (from main() there would be no one to reply to).
class status_monitor : public cas::actor {
public:
    static inline std::atomic<long long> pending{-1};  // observed by the tutorial

    explicit status_monitor(cas::actor_ref relay) : m_relay(std::move(relay)) {}

protected:
    void on_start() override {
        handler<zeromq_status>(&status_monitor::on_status);
        m_relay.tell(zeromq_status{});
    }

private:
    void on_status(const zeromq_status& msg) {
        std::cout << "  monitor: " << msg.mode << " on " << msg.endpoint << ", " << msg.route_count
                  << " typed route(s), " << msg.pending_count << " pending request(s)\n";
        pending = static_cast<long long>(msg.pending_count);
    }

    cas::actor_ref m_relay;
};

} // namespace

namespace tut {

bool run_zeromq_relay() {
    bool ok = true;

    heading("Two relays, an exchange and a trader");
    //----------------------------------------------
    auto server_relay = cas::system::create<cas::zeromq_relay_actor>();
    auto client_relay = cas::system::create<cas::zeromq_relay_actor>();
    auto exchange_ref = cas::system::create<exchange>(server_relay);
    auto trader_ref = cas::system::create<trader>(client_relay);
    cas::system::start();

    heading("1. The server binds, then the client connects");
    //-------------------------------------------------------
    exchange_ref.tell(exchange::listen{});
    ok &= check(wait_until(exchange::bound), "the exchange's relay is bound");
    trader_ref.tell(trader::connect{});
    ok &= check(wait_until(trader::connected), "the trader's relay is connected");

    heading("2. The trader sends an order; the exchange acknowledges it");
    //--------------------------------------------------------------------
    trader_ref.tell(trader::place_order{});
    ok &= check(wait_until(exchange::orders, 1),
                "the exchange received the order (routed by message_type)");
    ok &= check(wait_until(trader::acks, 1),
                "the ack came back to the trader (matched by correlation id)");
    ok &= check(trader::last_ack() == std::string("ACK ") + trader::order_text, "with the exchange's payload");

    heading("3. The exchange pushes market data");
    //--------------------------------------------
    exchange_ref.tell(exchange::push_market_data{});
    ok &= check(wait_until(trader::market_data, 1),
                "the trader got it through its default route");

    heading("4. Asking the client relay for its status");
    //---------------------------------------------------
    cas::system::create<status_monitor>(client_relay);
    ok &= check(wait_until(status_monitor::pending, 0),
                "no request is still waiting for a reply");

    heading("5. Binding twice is reported to the sender");
    //----------------------------------------------------
    exchange_ref.tell(exchange::bind_again{});
    ok &= check(wait_until(exchange::errors, 1), "the exchange received a zeromq_error");

    heading("Shutting down");
    //-----------------------
    cas::system::shutdown();
    cas::system::wait_for_shutdown();
    return ok;
}

} // namespace tut
