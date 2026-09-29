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
//   plain actor, every order handler would check the state and park orders by
//   hand. A stateful_actor does it for you: while disconnected or logging in it
//   does not accept new_order, so orders wait in its queue - in the order they
//   were sent - and are handled the moment the session reaches a state that
//   accepts them.

#include "tutorials.h"
#include "cas/cas.h"

#include <atomic>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

namespace {

// A template only so that part 5 can run the same class on a dedicated thread;
// in your own code, derive from cas::stateful_actor or cas::stateful_fast_actor.
// Each instantiation has its own nested messages and its own static members.
template<typename Base>
class exchange_session : public Base {
public:
    struct connect : cas::message_base {};
    struct login_ack : cas::message_base {};
    struct disconnect : cas::message_base {};
    struct new_order : cas::message_base {
        int order_id = 0;
    };
    struct backlog_query {};  // call() tag: how many messages wait (queued + held)?

    static void send_order(const cas::actor_ref& to, int id) {
        new_order order;
        order.order_id = id;
        to.tell(order);
    }

    // Observed by the tutorial: the order ids actually sent, in sending order
    static std::vector<int> orders_sent() {
        std::lock_guard<std::mutex> lock(s_mutex);
        return s_orders_sent;
    }

protected:
    void on_start() override {
        // Handlers for every message the session ever handles. Which of them
        // is accepted *right now* is decided separately, by the state.
        this->template handler<connect>(&exchange_session::on_connect);
        this->template handler<login_ack>(&exchange_session::on_login_ack);
        this->template handler<disconnect>(&exchange_session::on_disconnect);
        this->template handler<new_order>(&exchange_session::on_new_order);
        // Calls are never held, whatever the state: a monitor can always ask
        this->template call_handler<size_t, backlog_query>(&exchange_session::on_backlog);
        enter_disconnected();
    }

private:
    // States. A stateful actor starts out accepting everything, so "only
    // these" is reject_all then accept (reject_message_type<T>() alone would
    // do nothing while everything is accepted).
    void enter_disconnected() {
        std::cout << "  session: disconnected\n";
        this->reject_all_message_types();
        this->template accept_message_type<connect>();  // new_order now held
    }

    void enter_logging_in() {
        std::cout << "  session: logging in\n";
        this->reject_all_message_types();
        this->template accept_message_type<login_ack>();
    }

    void enter_logged_in() {
        std::cout << "  session: logged in\n";
        this->reject_all_message_types();
        this->template accept_message_type<new_order>();  // held orders released, oldest first
        this->template accept_message_type<disconnect>();
    }

    // Handlers. A real session would open the socket and send the logon in
    // on_connect; on_new_order only ever runs when logged in.
    void on_connect(const connect&) { enter_logging_in(); }
    void on_login_ack(const login_ack&) { enter_logged_in(); }
    void on_disconnect(const disconnect&) { enter_disconnected(); }

    void on_new_order(const new_order& msg) {
        std::cout << "  session: sending order " << msg.order_id << "\n";
        std::lock_guard<std::mutex> lock(s_mutex);
        s_orders_sent.push_back(msg.order_id);
    }

    size_t on_backlog() { return this->queue_size(); }  // queued + held

    static inline std::mutex s_mutex;
    static inline std::vector<int> s_orders_sent;
};

using session = exchange_session<cas::stateful_actor>;
using fast_session = exchange_session<cas::stateful_fast_actor>;

} // namespace

namespace tut {

bool run_stateful_actor() {
    bool ok = true;

    std::atomic<int> dead_letters{0};
    cas::system::set_dead_letter_handler([&dead_letters](const cas::dead_letter_info& info) {
        std::cout << "  [dead letter] " << info.message_type.name() << "\n";
        dead_letters.fetch_add(1);
    });

    auto s = cas::system::create<session>();
    cas::system::start();

    heading("1. Orders sent while disconnected are held");
    session::send_order(s, 1);
    session::send_order(s, 2);
    // Answered although new_order is not accepted: calls are never held
    ok &= check(s.call<size_t>(session::backlog_query{}) == 2, "both orders are held (call() still answered)");
    ok &= check(session::orders_sent().empty(), "no order was sent before login");

    heading("2. Connecting and logging in releases them, oldest first");
    s.tell(session::connect{});
    session::send_order(s, 3);  // still not logged in: held behind 1 and 2
    s.tell(session::login_ack{});
    ok &= check(wait_until([] { return session::orders_sent().size() == 3; }),
                "all three orders were sent once logged in");
    ok &= check(session::orders_sent() == std::vector<int>{1, 2, 3}, "in the order they were sent");

    heading("3. While logged in, orders go straight through");
    session::send_order(s, 4);
    ok &= check(wait_until([] { return session::orders_sent().size() == 4; }),
                "an order sent while logged in is handled at once");

    heading("4. Disconnecting, then stopping with an order still held");
    s.tell(session::disconnect{});
    session::send_order(s, 5);  // arrives after disconnect: held
    ok &= check(wait_until([&] { return s.call<size_t>(session::backlog_query{}) == 1; }),
                "the order sent after disconnecting is held");
    cas::system::stop_actor(s);
    ok &= check(dead_letters.load() == 1, "the held order became a dead letter when the session stopped");
    ok &= check(session::orders_sent().size() == 4, "it was never sent");

    heading("5. The same on a dedicated thread: stateful_fast_actor");
    // Same API, same rules - for a session that is also latency-critical
    auto fs = cas::system::create<fast_session>();
    fast_session::send_order(fs, 10);
    fs.tell(fast_session::connect{});
    fast_session::send_order(fs, 11);
    fs.tell(fast_session::login_ack{});
    ok &= check(wait_until([] { return fast_session::orders_sent().size() == 2; }),
                "the fast session held its orders until login too");
    ok &= check(fast_session::orders_sent() == std::vector<int>{10, 11}, "in order");

    heading("Shutting down");
    cas::system::shutdown();
    cas::system::wait_for_shutdown();
    cas::system::clear_dead_letter_handler();
    return ok;
}

} // namespace tut
