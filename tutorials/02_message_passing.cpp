// Tutorial 2: Message passing
//
// TEACHES
//   - one actor handling several message types
//   - actor state, and why it needs no locking
//   - handlers as member functions: on_start() lists what the actor accepts
//   - what happens to a message with no registered handler
//
// ASSUMES
//   Tutorial 1.
//
// THE IDEA
//   Choose an actor by *what it owns*. Give it the data and let every
//   operation on that data arrive as a message: the framework serialises the
//   operations for you, so the data needs no synchronisation.

#include "tutorials.h"
#include "cas/cas.h"

#include <atomic>
#include <iostream>

namespace {

// The messages. One small message per operation - messages are copied into
// the mailbox.
namespace msg {

struct deposit : cas::message_base {
    int amount = 0;
};

struct withdraw : cas::message_base {
    int amount = 0;
};

struct print_balance : cas::message_base {};

// Sent by main() although the account has no handler for it.
struct unsupported : cas::message_base {};

} // namespace msg

// The actors.
namespace actor {

class account : public cas::actor {
public:
    // Build and send in one line, so the caller does not repeat the fields
    static void send_deposit(const cas::actor_ref& to, int amount) {
        msg::deposit out;
        out.amount = amount;
        to.tell(out);
    }
    static void send_withdraw(const cas::actor_ref& to, int amount) {
        msg::withdraw out;
        out.amount = amount;
        to.tell(out);
    }

    // Observed by the tutorial.
    static inline std::atomic<int> s_rejected_count{0};
    static inline std::atomic<int> s_unhandled_count{0};
    static inline std::atomic<int> s_final_balance{0};

protected:
    void on_start() override {
        set_name("account");
        // One line per message type: on_start() reads as the list of
        // messages this actor accepts.
        handler<msg::deposit>(&account::on_deposit);
        handler<msg::withdraw>(&account::on_withdraw);
        handler<msg::print_balance>(&account::on_print_balance);
    }

    // A message whose type was never registered lands here instead of
    // throwing. The default does nothing, so override it to catch a handler
    // you forgot to register.
    void on_unhandled_message(cas::message_base*) override {
        std::cout << "  (an unregistered message type arrived)\n";
        s_unhandled_count.fetch_add(1);
    }

    // Runs after the last message: a safe moment to publish final state.
    void on_stop() override { s_final_balance.store(m_balance); }

private:
    void on_deposit(const msg::deposit& _msg) {
        m_balance += _msg.amount;
        std::cout << "  deposit " << _msg.amount << " -> balance " << m_balance << "\n";
    }

    void on_withdraw(const msg::withdraw& _msg) {
        if (_msg.amount > m_balance) {
            std::cout << "  withdraw " << _msg.amount << " REJECTED\n";
            s_rejected_count.fetch_add(1);
            return;
        }
        m_balance -= _msg.amount;
        std::cout << "  withdraw " << _msg.amount << " -> balance " << m_balance << "\n";
    }

    void on_print_balance(const msg::print_balance&) {
        std::cout << "  balance: " << m_balance << "\n";
    }

    // A plain int: handlers run one at a time on one thread, so deposits and
    // withdrawals can never interleave. No lock, no atomic.
    int m_balance = 0;
};

} // namespace actor

} // namespace

namespace tut {

bool run_message_passing() {
    bool ok = true;

    heading("Starting the system");
    //-----------------------------
    auto account = cas::system::create<actor::account>();
    cas::system::start();

    heading("Sending messages");
    //--------------------------
    // Messages from one sender to one actor arrive in the order sent, so this
    // sequence is deterministic.
    actor::account::send_deposit(account, 100);
    actor::account::send_deposit(account, 50);
    actor::account::send_withdraw(account, 30);
    actor::account::send_withdraw(account, 500);  // more than the balance: rejected
    account.tell(msg::print_balance{});
    ok &= check(wait_until(actor::account::s_rejected_count, 1),
                "over-balance withdrawal was rejected");

    heading("Sending an unregistered message type");
    //----------------------------------------------
    // Any message type can be sent; account simply has no handler for this one.
    account.tell(msg::unsupported{});
    ok &= check(wait_until(actor::account::s_unhandled_count, 1),
                "unhandled message reached on_unhandled_message()");

    heading("Shutting down");
    //-----------------------
    // wait_for_shutdown() guarantees on_stop() has run before we read its result.
    cas::system::shutdown();
    cas::system::wait_for_shutdown();
    ok &= check(actor::account::s_final_balance.load() == 120,
                "final balance is 120 (100 + 50 - 30, one rejected)");

    return ok;
}

} // namespace tut
