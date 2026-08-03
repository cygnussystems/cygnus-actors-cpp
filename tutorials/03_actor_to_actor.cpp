// Tutorial 3: Actors talking to each other
//
// TEACHES
//   - giving one actor a reference to another
//   - replying with msg.sender, and why it must be checked
//   - self(), and why an actor needs it to be replied to
//   - a bounded ping/pong exchange
//
// ASSUMES
//   Tutorials 1-2: messages, actors, handlers, actor state.
//
// THE IDEA
//   Everything so far has been main() talking to one actor. Real systems are
//   actors talking to each other. The only new mechanism is that a message
//   carries a reference back to whoever sent it - if the sender was an actor.

#include "tutorials.h"
#include "cas/cas.h"

#include <atomic>
#include <iostream>

namespace {

// --- 1. A request and its reply ----------------------------------------------
//
// Two message types: one each way. There is no built-in pairing between them;
// a reply is just another message that happens to travel in the other
// direction.

struct ping : cas::message_base {
    int round = 0;
};

struct pong : cas::message_base {
    int round = 0;
};

// Kicks off the exchange. Sent from main(), which is not an actor.
struct start_exchange : cas::message_base {};

constexpr int k_rounds = 3;

std::atomic<int> g_pings_handled{0};
std::atomic<int> g_pongs_handled{0};
std::atomic<int> g_replies_without_sender{0};

// --- 2. The responder --------------------------------------------------------
//
// This actor never needs to know who it is talking to. It replies to whoever
// sent the message, using the sender reference the framework filled in.

class ponger : public cas::actor {
protected:
    void on_start() override {
        set_name("ponger");
        handler<ping>(&ponger::on_ping);
    }

    void on_ping(const ping& msg) {
        std::cout << "  ponger  <- ping  round " << msg.round << "\n";
        g_pings_handled.fetch_add(1);

        // msg.sender is only valid when an *actor* sent the message. A tell()
        // from main() (or any non-actor thread) leaves it empty, so always
        // check before replying - otherwise the reply goes nowhere and the
        // exchange silently stalls.
        if (!msg.sender.is_valid()) {
            g_replies_without_sender.fetch_add(1);
            std::cout << "  ponger: no sender to reply to\n";
            return;
        }

        pong reply;
        reply.round = msg.round;
        msg.sender.tell(reply);
    }
};

// --- 3. The initiator --------------------------------------------------------
//
// Holds a reference to its partner, and counts rounds so the exchange
// terminates. Two actors replying to each other forever is a livelock that
// keeps a worker thread busy indefinitely - always give an exchange a stop
// condition.

class pinger : public cas::actor {
public:
    // Constructor arguments passed to system::create<pinger>(...) are
    // forwarded here. This is how an actor learns about its collaborators.
    explicit pinger(cas::actor_ref partner) : m_partner(partner) {}

protected:
    void on_start() override {
        set_name("pinger");
        handler<start_exchange>(&pinger::on_start_exchange);
        handler<pong>(&pinger::on_pong);
    }

    void on_start_exchange(const start_exchange&) {
        send_ping(1);
    }

    void on_pong(const pong& msg) {
        std::cout << "  pinger  <- pong  round " << msg.round << "\n";
        g_pongs_handled.fetch_add(1);

        if (msg.round < k_rounds) {
            send_ping(msg.round + 1);
        }
    }

private:
    void send_ping(int round) {
        ping msg;
        msg.round = round;

        // Nothing special is needed to be replied to. The framework fills in
        // msg.sender from the actor currently running on this thread, so a
        // tell() issued from inside a handler is automatically attributed to
        // this actor.
        //
        // That is exactly why the same tell() from main() arrives with no
        // sender: there is no current actor to attribute it to.
        m_partner.tell(msg);
    }

    cas::actor_ref m_partner;
};

} // namespace

namespace tut {

bool run_actor_to_actor() {
    bool ok = true;

    section("Creating two actors");

    // Create the responder first, then hand its reference to the initiator.
    auto ponger_ref = cas::system::create<ponger>();
    auto pinger_ref = cas::system::create<pinger>(ponger_ref);

    cas::system::start();

    // --- 4. Kick it off ------------------------------------------------------
    //
    // main() is not an actor, so this send carries no sender. That is fine:
    // start_exchange needs no reply. It is also why pinger cannot simply be
    // told to reply to us.

    section("Running the exchange");
    pinger_ref.tell(start_exchange{});

    ok &= check(wait_until([] { return g_pongs_handled.load() == k_rounds; }),
                "completed 3 full ping/pong rounds");
    ok &= check(g_pings_handled.load() == k_rounds,
                "ponger handled one ping per round");

    section("Shutting down");
    cas::system::shutdown();
    cas::system::wait_for_shutdown();

    ok &= check(g_replies_without_sender.load() == 0,
                "every ping arrived with a valid sender");

    return ok;
}

} // namespace tut
