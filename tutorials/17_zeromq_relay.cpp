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

// The messages. Only the commands main() gives the actors: the relay's own
// control and data messages (bind, connect, route, send, recv, status, error)
// come with the framework, as cas::msg::zeromq_*.
namespace msg {

struct listen : cas::message_base {};            // to the exchange: route, then bind
struct push_market_data : cas::message_base {};  // to the exchange
struct bind_again : cas::message_base {};        // to the exchange: provoke an error

struct connect : cas::message_base {};      // to the trader
struct place_order : cas::message_base {};  // to the trader

} // namespace msg

// Payloads are bytes; these tutorial payloads are text.
std::vector<char> bytes(const std::string& s) { return {s.begin(), s.end()}; }
std::string text(const std::vector<char>& data) { return {data.begin(), data.end()}; }

// The actors.
namespace actor {

// Server side. Owns the server relay and sends every control message from a
// handler, so it is their sender: status replies and errors come back here.
class exchange : public cas::actor {
public:
    static constexpr const char* s_endpoint = "inproc://tutorial-exchange";  // e.g. "tcp://*:5555"

    static inline std::atomic<bool> s_bound{false};  // observed by the tutorial
    static inline std::atomic<int> s_order_count{0};
    static inline std::atomic<int> s_error_count{0};

    explicit exchange(cas::actor_ref relay) : m_relay(std::move(relay)) {}

protected:
    void on_start() override {
        set_name("exchange");
        handler<msg::listen>(&exchange::on_listen);
        handler<msg::push_market_data>(&exchange::on_push);
        handler<msg::bind_again>(&exchange::on_bind_again);
        handler<cas::msg::zeromq_recv>(&exchange::on_recv);
        handler<cas::msg::zeromq_status>(&exchange::on_status);
        handler<cas::msg::zeromq_error>(&exchange::on_error);
    }

private:
    // Route first, then bind: messages arriving right after the bind already
    // have somewhere to go. Inbound "NewOrderSingle" frames come to this actor.
    void on_listen(const msg::listen&) {
        cas::msg::zeromq_route route;
        route.message_type = "NewOrderSingle";
        route.target = self();
        m_relay.tell(route);
        bind();
    }

    // Binding twice is an error - reported to us, the sender
    void on_bind_again(const msg::bind_again&) { bind(); }

    void bind() {
        cas::msg::zeromq_bind bind_request;
        bind_request.endpoint = s_endpoint;
        m_relay.tell(bind_request);
    }

    // After a successful bind the relay reports its status to the sender
    void on_status(const cas::msg::zeromq_status& _msg) {
        if (_msg.connected && _msg.mode == "router") {
            std::cout << "  exchange: listening on " << _msg.endpoint << "\n";
            s_bound = true;
        }
    }

    // Reply to that one client: a ROUTER needs the target identity. The
    // correlation id is echoed so the client relay can hand the reply to
    // whoever sent the request.
    void on_recv(const cas::msg::zeromq_recv& _msg) {
        std::cout << "  exchange: " << _msg.message_type << " from '" << _msg.source
                  << "': " << text(_msg.data) << "\n";
        s_order_count.fetch_add(1);
        m_client = _msg.source;  // remembered for pushes later
        cas::msg::zeromq_send ack;
        ack.target = _msg.source;
        ack.correlation_id = _msg.correlation_id;
        ack.content_type = "text";
        ack.message_type = "OrderAck";
        ack.data = bytes("ACK " + text(_msg.data));
        m_relay.tell(ack);
    }

    // Unsolicited: no correlation id, so the client routes it by type
    void on_push(const msg::push_market_data&) {
        cas::msg::zeromq_send md;
        md.target = m_client;
        md.content_type = "text";
        md.message_type = "MarketData";
        md.data = bytes("ES 5321.25");
        m_relay.tell(md);
    }

    void on_error(const cas::msg::zeromq_error& _msg) {
        std::cout << "  exchange: relay error: " << _msg.message << "\n";
        s_error_count.fetch_add(1);
    }

    cas::actor_ref m_relay;
    std::string m_client;
};

// Client side.
class trader : public cas::actor {
public:
    static constexpr const char* s_order_text = "BUY 2 ES @ 5321.00";

    static inline std::atomic<bool> s_connected{false};     // observed by the tutorial
    static inline std::atomic<int> s_ack_count{0};          // matched by correlation id
    static inline std::atomic<int> s_market_data_count{0};  // via the default route

    static std::string last_ack() {
        std::lock_guard<std::mutex> lock(s_mutex);
        return s_last_ack;
    }

    explicit trader(cas::actor_ref relay) : m_relay(std::move(relay)) {}

protected:
    void on_start() override {
        set_name("trader");
        handler<msg::connect>(&trader::on_connect);
        handler<msg::place_order>(&trader::on_place_order);
        handler<cas::msg::zeromq_recv>(&trader::on_recv);
        handler<cas::msg::zeromq_status>(&trader::on_status);
    }

private:
    // Default route (empty message_type): everything that is neither a reply
    // to one of our requests nor routed by type comes here
    void on_connect(const msg::connect&) {
        cas::msg::zeromq_route route;
        route.target = self();
        m_relay.tell(route);

        cas::msg::zeromq_connect conn;
        conn.endpoint = exchange::s_endpoint;  // e.g. "tcp://exchange-host:5555"
        conn.identity = "trader-1";            // how the exchange addresses us
        m_relay.tell(conn);
    }

    void on_status(const cas::msg::zeromq_status& _msg) {
        if (_msg.mode == "dealer" && _msg.connected) s_connected = true;
    }

    // Request: a non-zero correlation id, sent from inside this actor, makes
    // the client relay remember us as the requester; the reply comes back
    // here whatever routes are set.
    void on_place_order(const msg::place_order&) {
        cas::msg::zeromq_send order;
        order.correlation_id = 1;
        order.content_type = "text";
        order.message_type = "NewOrderSingle";
        order.data = bytes(s_order_text);
        m_relay.tell(order);
    }

    void on_recv(const cas::msg::zeromq_recv& _msg) {
        std::cout << "  trader: " << _msg.message_type << ": " << text(_msg.data) << "\n";
        if (_msg.message_type == "OrderAck" && _msg.correlation_id == 1) {
            {
                std::lock_guard<std::mutex> lock(s_mutex);
                s_last_ack = text(_msg.data);
            }
            s_ack_count.fetch_add(1);
        } else if (_msg.message_type == "MarketData") {
            s_market_data_count.fetch_add(1);
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
    static inline std::atomic<long long> s_pending_count{-1};  // observed by the tutorial

    explicit status_monitor(cas::actor_ref relay) : m_relay(std::move(relay)) {}

protected:
    void on_start() override {
        handler<cas::msg::zeromq_status>(&status_monitor::on_status);
        m_relay.tell(cas::msg::zeromq_status{});
    }

private:
    void on_status(const cas::msg::zeromq_status& _msg) {
        std::cout << "  monitor: " << _msg.mode << " on " << _msg.endpoint << ", " << _msg.route_count
                  << " typed route(s), " << _msg.pending_count << " pending request(s)\n";
        s_pending_count = static_cast<long long>(_msg.pending_count);
    }

    cas::actor_ref m_relay;
};

} // namespace actor

} // namespace

namespace tut {

bool run_zeromq_relay() {
    bool ok = true;

    heading("Two relays, an exchange and a trader");
    //----------------------------------------------
    auto server_relay = cas::system::create<cas::zeromq_relay_actor>();
    auto client_relay = cas::system::create<cas::zeromq_relay_actor>();
    auto exchange_ref = cas::system::create<actor::exchange>(server_relay);
    auto trader_ref = cas::system::create<actor::trader>(client_relay);
    cas::system::start();

    heading("1. The server binds, then the client connects");
    //-------------------------------------------------------
    exchange_ref.tell(msg::listen{});
    ok &= check(wait_until(actor::exchange::s_bound), "the exchange's relay is bound");
    trader_ref.tell(msg::connect{});
    ok &= check(wait_until(actor::trader::s_connected), "the trader's relay is connected");

    heading("2. The trader sends an order; the exchange acknowledges it");
    //--------------------------------------------------------------------
    trader_ref.tell(msg::place_order{});
    ok &= check(wait_until(actor::exchange::s_order_count, 1),
                "the exchange received the order (routed by message_type)");
    ok &= check(wait_until(actor::trader::s_ack_count, 1),
                "the ack came back to the trader (matched by correlation id)");
    ok &= check(actor::trader::last_ack() == std::string("ACK ") + actor::trader::s_order_text,
                "with the exchange's payload");

    heading("3. The exchange pushes market data");
    //--------------------------------------------
    exchange_ref.tell(msg::push_market_data{});
    ok &= check(wait_until(actor::trader::s_market_data_count, 1),
                "the trader got it through its default route");

    heading("4. Asking the client relay for its status");
    //---------------------------------------------------
    cas::system::create<actor::status_monitor>(client_relay);
    ok &= check(wait_until(actor::status_monitor::s_pending_count, 0),
                "no request is still waiting for a reply");

    heading("5. Binding twice is reported to the sender");
    //----------------------------------------------------
    exchange_ref.tell(msg::bind_again{});
    ok &= check(wait_until(actor::exchange::s_error_count, 1), "the exchange received a zeromq_error");

    heading("Shutting down");
    //-----------------------
    cas::system::shutdown();
    cas::system::wait_for_shutdown();
    return ok;
}

} // namespace tut
