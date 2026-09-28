// Tutorial 16: Stateful actors - selective receive
//
// TEACHES
//   - stateful_actor: in each state, accept only some message types
//   - messages of other types are held, in order, not lost or reordered
//   - changing state releases held messages, oldest first
//   - call() is never held (so you can always ask a busy state machine)
//   - messages still held when the actor stops become dead letters
//   - stateful_fast_actor: the same, on a dedicated thread
//
// ASSUMES
//   Tutorials 1-10 (6 for call(), 10 for dead letters), and 13 for fast actors.
//
// THE IDEA
//   An exchange session must not send orders before it is logged in. With a
//   plain actor, every order handler would check the state and park orders
//   somewhere by hand. A stateful_actor does that for you: while
//   disconnected or logging in it simply does not accept new_order, so orders
//   wait in its queue - in the order they were sent - and are handled the
//   moment the session reaches a state that accepts them.

#include "tutorials.h"
#include "cas/cas.h"

#include <atomic>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

namespace {

// --- Messages ------------------------------------------------------------------

struct connect_cmd : cas::message_base {};
struct login_ack : cas::message_base {};
struct disconnect_cmd : cas::message_base {};
struct new_order : cas::message_base {
    int order_id = 0;
};

// call() tag: how many messages are waiting (queued + held)?
struct backlog_op {};

// --- What main() observes ------------------------------------------------------

std::mutex g_mutex;
std::vector<int> g_orders_sent;  // guarded by g_mutex, in the order they were sent
std::atomic<int> g_logins{0};
std::atomic<int> g_dead_letters{0};

std::vector<int> orders_sent() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_orders_sent;
}

// --- The session ------------------------------------------------------------------
//
// A template only so that part 5 can run the same class on a dedicated thread.
// In your own code, derive from cas::stateful_actor or cas::stateful_fast_actor.

template<typename Base>
class exchange_session : public Base {
protected:
    void on_start() override {
        // Handlers for every message the session ever handles. Which of them
        // is accepted *right now* is decided separately, by the state.
        this->template handler<connect_cmd>(&exchange_session::on_connect);
        this->template handler<login_ack>(&exchange_session::on_login_ack);
        this->template handler<disconnect_cmd>(&exchange_session::on_disconnect);
        this->template handler<new_order>(&exchange_session::on_new_order);

        // Calls are never held, whatever the state: a monitor can always ask
        this->template call_handler<size_t, backlog_op>(&exchange_session::backlog);

        enter_disconnected();
    }

    // --- States ----------------------------------------------------------------
    //
    // A stateful actor starts out accepting everything. reject_all then accept
    // is the pattern for "only these": note that reject_message_type<T>() on
    // its own does nothing while everything is accepted.

    void enter_disconnected() {
        std::cout << "  session: disconnected\n";
        this->reject_all_message_types();
        this->template accept_message_type<connect_cmd>();
        // new_order is not accepted: orders arriving now are held
    }

    void enter_logging_in() {
        std::cout << "  session: logging in\n";
        this->reject_all_message_types();
        this->template accept_message_type<login_ack>();
    }

    void enter_logged_in() {
        std::cout << "  session: logged in\n";
        this->reject_all_message_types();
        this->template accept_message_type<new_order>();
        this->template accept_message_type<disconnect_cmd>();
        // Held orders are released now, oldest first - no code needed
    }

    // --- Handlers ----------------------------------------------------------------

    void on_connect(const connect_cmd&) {
        // A real session would open the socket and send the logon here
        enter_logging_in();
    }

    void on_login_ack(const login_ack&) {
        g_logins.fetch_add(1);
        enter_logged_in();
    }

    void on_disconnect(const disconnect_cmd&) {
        enter_disconnected();
    }

    void on_new_order(const new_order& msg) {
        // Only ever runs when logged in: the state guarantees it
        std::cout << "  session: sending order " << msg.order_id << "\n";
        std::lock_guard<std::mutex> lock(g_mutex);
        g_orders_sent.push_back(msg.order_id);
    }

    size_t backlog() {
        return this->queue_size();  // queued + held
    }
};

using session = exchange_session<cas::stateful_actor>;
using fast_session = exchange_session<cas::stateful_fast_actor>;

void send_order(const cas::actor_ref& to, int id) {
    new_order order;
    order.order_id = id;
    to.tell(order);
}

} // namespace

namespace tut {

bool run_stateful_actor() {
    bool ok = true;

    cas::system::set_dead_letter_handler([](const cas::dead_letter_info& info) {
        std::cout << "  [dead letter] " << info.message_type.name() << "\n";
        g_dead_letters.fetch_add(1);
    });

    auto s = cas::system::create<session>();
    cas::system::start();

    // --- 1. Orders before login are held, not handled and not lost ----------------

    section("Orders sent while disconnected");
    send_order(s, 1);
    send_order(s, 2);

    // call() is answered even though new_order is not accepted: calls are
    // never held. It also shows the two orders waiting.
    ok &= check(s.call<size_t>(backlog_op{}) == 2, "both orders are held (call() still answered)");
    ok &= check(orders_sent().empty(), "no order was sent before login");

    // --- 2. State changes release them, oldest first --------------------------------

    section("Connecting and logging in");
    s.tell(connect_cmd{});
    send_order(s, 3);          // still not logged in: held behind 1 and 2
    s.tell(login_ack{});

    ok &= check(wait_until([] { return orders_sent().size() == 3; }),
                "all three orders were sent once logged in");
    ok &= check(orders_sent() == std::vector<int>{1, 2, 3},
                "in the order they were sent");

    // --- 3. While logged in, orders go straight through ------------------------------

    section("Trading");
    send_order(s, 4);
    ok &= check(wait_until([] { return orders_sent().size() == 4; }),
                "an order sent while logged in is handled at once");

    // --- 4. Disconnect: held again; stopping turns held messages into dead letters ---

    section("Disconnecting, then stopping with an order still held");
    s.tell(disconnect_cmd{});
    send_order(s, 5);          // arrives after disconnect: held
    ok &= check(wait_until([&] { return s.call<size_t>(backlog_op{}) == 1; }),
                "the order sent after disconnecting is held");

    cas::system::stop_actor(s);
    ok &= check(g_dead_letters.load() == 1,
                "the held order became a dead letter when the session stopped");
    ok &= check(orders_sent().size() == 4, "it was never sent");

    // --- 5. The same on a dedicated thread: stateful_fast_actor -----------------------
    //
    // Same API, same rules - for a session that is also latency-critical.

    section("stateful_fast_actor");
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_orders_sent.clear();
    }
    auto fs = cas::system::create<fast_session>();
    send_order(fs, 10);
    fs.tell(connect_cmd{});
    send_order(fs, 11);
    fs.tell(login_ack{});
    ok &= check(wait_until([] { return orders_sent().size() == 2; }),
                "the fast session held its orders until login too");
    ok &= check(orders_sent() == std::vector<int>{10, 11}, "in order");

    section("Shutting down");
    cas::system::shutdown();
    cas::system::wait_for_shutdown();
    cas::system::clear_dead_letter_handler();

    return ok;
}

} // namespace tut
