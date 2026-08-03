// Tutorial 2: Message passing
//
// TEACHES
//   - one actor handling several message types
//   - actor state, and why it needs no locking
//   - member-function handlers (the default) vs lambda handlers
//   - what happens to a message with no registered handler
//
// ASSUMES
//   Tutorial 1: messages, actors, create/start/tell/shutdown.
//
// THE IDEA
//   An actor is picked by *what it owns*, not by what it does. Give one actor
//   the data and let every operation on that data arrive as a message; the
//   framework then serialises those operations for you.

#include "tutorials.h"
#include "cas/cas.h"

#include <atomic>
#include <iostream>
#include <string>

namespace {

// --- 1. Several message types ------------------------------------------------
//
// One struct per operation. Keep them small: messages are copied into the
// mailbox, so a message is a request, not a place to park a large object.

struct deposit : cas::message_base {
    int amount = 0;
};

struct withdraw : cas::message_base {
    int amount = 0;
};

struct print_balance : cas::message_base {};

// A message type this actor deliberately never registers a handler for, used
// in section 4 to show what happens then.
struct unsupported : cas::message_base {};

std::atomic<int> g_final_balance{0};
std::atomic<int> g_rejected_withdrawals{0};
std::atomic<int> g_unhandled_count{0};

// --- 2. An actor that owns some state ----------------------------------------
//
// m_balance is ordinary, non-atomic state, and that is the point: only the
// actor's own thread ever touches it, and only one message is processed at a
// time. Deposits and withdrawals cannot interleave, so there is no lost
// update to guard against and no mutex to take.
//
// This is the core trade the actor model makes: you give up calling methods
// directly, and you get data that needs no synchronisation.

class account : public cas::actor {
protected:
    void on_start() override {
        set_name("account");

        // Registration reads as the actor's accepted-message list. Prefer
        // member functions for this: the logic lives in named methods below
        // rather than inline, so on_start() stays a summary.
        handler<deposit>(&account::on_deposit);
        handler<withdraw>(&account::on_withdraw);

        // A lambda is worth it for a genuine one-liner, where a named method
        // would be more ceremony than substance. Note it captures `this` to
        // reach actor state - safe, because the lambda only ever runs on this
        // actor's thread.
        handler<print_balance>([this](const print_balance&) {
            std::cout << "  balance: " << m_balance << "\n";
        });
    }

    // Handlers run one at a time, on one thread. No locking needed.
    void on_deposit(const deposit& msg) {
        m_balance += msg.amount;
        std::cout << "  deposit " << msg.amount
                  << " -> balance " << m_balance << "\n";
    }

    void on_withdraw(const withdraw& msg) {
        if (msg.amount > m_balance) {
            // Rejecting is just a normal code path. Tutorial 3 shows how to
            // tell the *sender* about it; tutorial 6 how to return a value.
            std::cout << "  withdraw " << msg.amount << " REJECTED (balance "
                      << m_balance << ")\n";
            g_rejected_withdrawals.fetch_add(1);
            return;
        }
        m_balance -= msg.amount;
        std::cout << "  withdraw " << msg.amount
                  << " -> balance " << m_balance << "\n";
    }

    // --- 3. Messages with no handler -----------------------------------------
    //
    // A message whose type was never registered is not an error and does not
    // throw. It arrives here instead. Overriding this is the easiest way to
    // catch a message you forgot to register - the default does nothing, so
    // without it such a message vanishes silently.
    void on_unhandled_message(cas::message_base* /*msg*/) override {
        std::cout << "  (an unregistered message type arrived)\n";
        g_unhandled_count.fetch_add(1);
    }

    // Runs after the last message is processed. See the lifecycle tutorial.
    void on_stop() override {
        g_final_balance.store(m_balance);
    }

private:
    int m_balance = 0;  // Plain int - see the note above.
};

} // namespace

namespace tut {

bool run_message_passing() {
    bool ok = true;

    section("Starting the system");
    auto acct = cas::system::create<account>();
    cas::system::start();

    // --- 4. Send a sequence of messages --------------------------------------
    //
    // Messages from a single sender to a single actor are delivered in the
    // order they were sent, so this sequence is deterministic. (Messages from
    // *different* senders are not ordered relative to each other.)

    section("Sending messages");

    // Messages carry framework fields (sender, ids) inherited from
    // message_base, so they are not aggregates - fill the fields in rather
    // than using braced initialisation.
    deposit d1;
    d1.amount = 100;
    acct.tell(d1);

    deposit d2;
    d2.amount = 50;
    acct.tell(d2);

    withdraw w1;
    w1.amount = 30;
    acct.tell(w1);

    withdraw w2;
    w2.amount = 500;  // more than the balance: will be rejected
    acct.tell(w2);

    acct.tell(print_balance{});

    ok &= check(wait_until([] { return g_rejected_withdrawals.load() == 1; }),
                "over-balance withdrawal was rejected");

    // --- 5. A message nobody handles -----------------------------------------

    section("Sending an unregistered message type");
    acct.tell(unsupported{});
    ok &= check(wait_until([] { return g_unhandled_count.load() == 1; }),
                "unhandled message reached on_unhandled_message()");

    // --- 6. Shut down and check the result -----------------------------------
    //
    // Reading actor state is safe only once the actor has stopped. Here
    // on_stop() published the balance, and wait_for_shutdown() guarantees it
    // has run before we look.

    section("Shutting down");
    cas::system::shutdown();
    cas::system::wait_for_shutdown();

    // 100 + 50 - 30, with the 500 rejected.
    ok &= check(g_final_balance.load() == 120,
                "final balance is 120 (100 + 50 - 30, one rejected)");

    return ok;
}

} // namespace tut
