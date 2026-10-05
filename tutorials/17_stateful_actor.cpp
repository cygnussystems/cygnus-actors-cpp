// Tutorial 17: Stateful actors - selective receive
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
//   Tutorials 1-10 (6 for call(), 10 for dead letters), and 14 for fast actors.
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

// The messages. Both session types below understand the same ones.
namespace msg {

struct connect : cas::message_base {};
struct login_ack : cas::message_base {};
struct disconnect : cas::message_base {};

struct new_order : cas::message_base {
    int order_id = 0;
};

struct backlog_query {};  // call() tag: how many messages wait (queued + held)?

} // namespace msg

// The actors.
namespace actor {

// A template only so that part 5 can run the same class on a dedicated thread;
// in your own code, derive from cas::stateful_actor or cas::stateful_fast_actor.
// Each instantiation has its own static members, so each session type keeps
// its own record of the orders it sent.
template<typename Base>
class exchange_session : public Base {
public:
    static void send_order(const cas::actor_ref& to, int id) {
        msg::new_order order;
        order.order_id = id;
        to.tell(order);
    }

    // Observed by the tutorial: the order ids actually sent, in sending order
    static std::vector<int> orders_sent() {
        std::lock_guard<std::mutex> lock(s_mutex);
        return s_orders_sent;
    }
    static size_t order_count() { return orders_sent().size(); }
    static inline std::atomic<int> s_disconnect_count{0};

protected:
    void on_start() override {
        // Handlers for every message the session ever handles. Which of them
        // is accepted *right now* is decided separately, by the state.
        this->template handler<msg::connect>(&exchange_session::on_connect);
        this->template handler<msg::login_ack>(&exchange_session::on_login_ack);
        this->template handler<msg::disconnect>(&exchange_session::on_disconnect);
        this->template handler<msg::new_order>(&exchange_session::on_new_order);
        // Calls are never held, whatever the state: a monitor can always ask
        this->template call_handler<size_t, msg::backlog_query>(&exchange_session::on_backlog);
        enter_disconnected();
    }

private:
    // States. A stateful actor starts out accepting everything, so "only
    // these" is reject_all then accept (reject_message_type<T>() alone would
    // do nothing while everything is accepted).
    void enter_disconnected() {
        std::cout << "  session: disconnected\n";
        this->reject_all_message_types();
        this->template accept_message_type<msg::connect>();  // new_order now held
    }

    void enter_logging_in() {
        std::cout << "  session: logging in\n";
        this->reject_all_message_types();
        this->template accept_message_type<msg::login_ack>();
    }

    void enter_logged_in() {
        std::cout << "  session: logged in\n";
        this->reject_all_message_types();
        this->template accept_message_type<msg::new_order>();  // held orders released, oldest first
        this->template accept_message_type<msg::disconnect>();
    }

    // Handlers. A real session would open the socket and send the logon in
    // on_connect; on_new_order only ever runs when logged in.
    void on_connect(const msg::connect&) { enter_logging_in(); }
    void on_login_ack(const msg::login_ack&) { enter_logged_in(); }
    void on_disconnect(const msg::disconnect&) {
        enter_disconnected();
        s_disconnect_count.fetch_add(1);
    }

    void on_new_order(const msg::new_order& _msg) {
        std::cout << "  session: sending order " << _msg.order_id << "\n";
        std::lock_guard<std::mutex> lock(s_mutex);
        s_orders_sent.push_back(_msg.order_id);
    }

    size_t on_backlog() { return this->queue_size(); }  // queued + held

    static inline std::mutex s_mutex;
    static inline std::vector<int> s_orders_sent;
};

using session = exchange_session<cas::stateful_actor>;
using fast_session = exchange_session<cas::stateful_fast_actor>;

} // namespace actor

// Counts dead letters - here, the order still held when the session stops
struct dead_letter_log {
    static inline std::atomic<int> s_dead_letter_count{0};  // observed by the tutorial

    static void record(const cas::dead_letter_info& info) {
        std::cout << "  [dead letter] " << info.message_type.name() << "\n";
        s_dead_letter_count.fetch_add(1);
    }
};

} // namespace

namespace tut {

bool run_stateful_actor() {
    bool ok = true;

    cas::system::set_dead_letter_handler(&dead_letter_log::record);

    auto s = cas::system::create<actor::session>();
    cas::system::start();

    heading("1. Orders sent while disconnected are held");
    //----------------------------------------------------
    actor::session::send_order(s, 1);
    actor::session::send_order(s, 2);
    // Answered although new_order is not accepted: calls are never held
    ok &= check(s.call<size_t>(msg::backlog_query{}) == 2, "both orders are held (call() still answered)");
    ok &= check(actor::session::orders_sent().empty(), "no order was sent before login");

    heading("2. Connecting and logging in releases them, oldest first");
    //------------------------------------------------------------------
    s.tell(msg::connect{});
    actor::session::send_order(s, 3);  // still not logged in: held behind 1 and 2
    s.tell(msg::login_ack{});
    ok &= check(wait_until(&actor::session::order_count, 3),
                "all three orders were sent once logged in");
    ok &= check(actor::session::orders_sent() == std::vector<int>{1, 2, 3}, "in the order they were sent");

    heading("3. While logged in, orders go straight through");
    //--------------------------------------------------------
    actor::session::send_order(s, 4);
    ok &= check(wait_until(&actor::session::order_count, 4),
                "an order sent while logged in is handled at once");

    heading("4. Disconnecting, then stopping with an order still held");
    //------------------------------------------------------------------
    s.tell(msg::disconnect{});
    actor::session::send_order(s, 5);  // arrives after disconnect: held
    ok &= check(wait_until(actor::session::s_disconnect_count, 1) && s.call<size_t>(msg::backlog_query{}) == 1,
                "the order sent after disconnecting is held");
    cas::system::stop_actor(s);
    ok &= check(dead_letter_log::s_dead_letter_count.load() == 1, "the held order became a dead letter when the session stopped");
    ok &= check(actor::session::orders_sent().size() == 4, "it was never sent");

    heading("5. The same on a dedicated thread: stateful_fast_actor");
    //----------------------------------------------------------------
    // Same API, same rules - for a session that is also latency-critical
    auto fs = cas::system::create<actor::fast_session>();
    actor::fast_session::send_order(fs, 10);
    fs.tell(msg::connect{});
    actor::fast_session::send_order(fs, 11);
    fs.tell(msg::login_ack{});
    ok &= check(wait_until(&actor::fast_session::order_count, 2),
                "the fast session held its orders until login too");
    ok &= check(actor::fast_session::orders_sent() == std::vector<int>{10, 11}, "in order");

    heading("Shutting down");
    //-----------------------
    cas::system::shutdown();
    cas::system::wait_for_shutdown();
    cas::system::clear_dead_letter_handler();
    return ok;
}

} // namespace tut
