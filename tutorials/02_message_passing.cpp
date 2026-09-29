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

class account : public cas::actor {
public:
    // One small message per operation - messages are copied into the mailbox.
    struct deposit : cas::message_base {
        int amount = 0;
    };
    struct withdraw : cas::message_base {
        int amount = 0;
    };
    struct print_balance : cas::message_base {};

    // Build and send in one line, so the caller does not repeat the fields
    static void send_deposit(const cas::actor_ref& to, int amount) {
        deposit msg;
        msg.amount = amount;
        to.tell(msg);
    }
    static void send_withdraw(const cas::actor_ref& to, int amount) {
        withdraw msg;
        msg.amount = amount;
        to.tell(msg);
    }

    // Observed by the tutorial.
    static inline std::atomic<int> rejected{0};
    static inline std::atomic<int> unhandled{0};
    static inline std::atomic<int> final_balance{0};

protected:
    void on_start() override {
        set_name("account");
        // One line per message type: on_start() reads as the list of
        // messages this actor accepts.
        handler<deposit>(&account::on_deposit);
        handler<withdraw>(&account::on_withdraw);
        handler<print_balance>(&account::on_print_balance);
    }

    // A message whose type was never registered lands here instead of
    // throwing. The default does nothing, so override it to catch a handler
    // you forgot to register.
    void on_unhandled_message(cas::message_base*) override {
        std::cout << "  (an unregistered message type arrived)\n";
        unhandled.fetch_add(1);
    }

    // Runs after the last message: a safe moment to publish final state.
    void on_stop() override { final_balance.store(m_balance); }

private:
    void on_deposit(const deposit& msg) {
        m_balance += msg.amount;
        std::cout << "  deposit " << msg.amount << " -> balance " << m_balance << "\n";
    }

    void on_withdraw(const withdraw& msg) {
        if (msg.amount > m_balance) {
            std::cout << "  withdraw " << msg.amount << " REJECTED\n";
            rejected.fetch_add(1);
            return;
        }
        m_balance -= msg.amount;
        std::cout << "  withdraw " << msg.amount << " -> balance " << m_balance << "\n";
    }

    void on_print_balance(const print_balance&) {
        std::cout << "  balance: " << m_balance << "\n";
    }

    // A plain int: handlers run one at a time on one thread, so deposits and
    // withdrawals can never interleave. No lock, no atomic.
    int m_balance = 0;
};

} // namespace

namespace tut {

bool run_message_passing() {
    bool ok = true;

    heading("Starting the system");
    //-----------------------------
    auto acct = cas::system::create<account>();
    cas::system::start();

    heading("Sending messages");
    //--------------------------
    // Messages from one sender to one actor arrive in the order sent, so this
    // sequence is deterministic.
    account::send_deposit(acct, 100);
    account::send_deposit(acct, 50);
    account::send_withdraw(acct, 30);
    account::send_withdraw(acct, 500);  // more than the balance: rejected
    acct.tell(account::print_balance{});
    ok &= check(wait_until(account::rejected, 1),
                "over-balance withdrawal was rejected");

    heading("Sending an unregistered message type");
    //----------------------------------------------
    // Any message type can be sent; account simply has no handler for this one.
    struct unsupported : cas::message_base {};
    acct.tell(unsupported{});
    ok &= check(wait_until(account::unhandled, 1),
                "unhandled message reached on_unhandled_message()");

    heading("Shutting down");
    //-----------------------
    // wait_for_shutdown() guarantees on_stop() has run before we read its result.
    cas::system::shutdown();
    cas::system::wait_for_shutdown();
    ok &= check(account::final_balance.load() == 120,
                "final balance is 120 (100 + 50 - 30, one rejected)");

    return ok;
}

} // namespace tut
