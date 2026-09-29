// Tutorial 3: Actors talking to each other
//
// TEACHES
//   - giving one actor a reference to another (constructor argument)
//   - replying through msg.sender, and why it must be checked
//   - an exchange with a stop condition
//
// ASSUMES
//   Tutorials 1-2.
//
// THE IDEA
//   A message carries a reference to the actor that sent it. Replying is
//   just telling that reference - no ids, no lookups.

#include "tutorials.h"
#include "cas/cas.h"

#include <atomic>
#include <iostream>

namespace {

// The protocol between the two actors. A request and its reply are ordinary
// messages that travel in opposite directions; both sides need them, so they
// live here rather than inside one actor.
struct ping : cas::message_base {
    int round = 0;
};
struct pong : cas::message_base {
    int round = 0;
};

// Answers every ping to whoever sent it. It never needs to know its partner.
class ponger : public cas::actor {
public:
    static inline std::atomic<int> pings{0};            // observed by the tutorial
    static inline std::atomic<int> without_sender{0};

protected:
    void on_start() override {
        set_name("ponger");
        handler<ping>(&ponger::on_ping);
    }

private:
    void on_ping(const ping& msg) {
        std::cout << "  ponger <- ping " << msg.round << "\n";
        pings.fetch_add(1);

        // msg.sender is set only when an actor sent the message; a tell()
        // from main() has none. Check before replying.
        if (!msg.sender.is_valid()) {
            without_sender.fetch_add(1);
            return;
        }
        pong reply;
        reply.round = msg.round;
        msg.sender.tell(reply);
    }
};

// Starts the exchange and stops it after `rounds` - two actors answering each
// other forever would keep a thread busy for good.
class pinger : public cas::actor {
public:
    struct start : cas::message_base {};                 // from main()

    static constexpr int rounds = 3;
    static inline std::atomic<int> pongs{0};             // observed by the tutorial

    // Arguments to create<pinger>(...) are forwarded here: this is how an
    // actor gets to know its collaborators.
    explicit pinger(cas::actor_ref partner) : m_partner(std::move(partner)) {}

protected:
    void on_start() override {
        set_name("pinger");
        handler<start>(&pinger::on_begin);
        handler<pong>(&pinger::on_pong);
    }

private:
    void on_begin(const start&) { send_ping(1); }

    void on_pong(const pong& msg) {
        std::cout << "  pinger <- pong " << msg.round << "\n";
        pongs.fetch_add(1);
        if (msg.round < rounds) {
            send_ping(msg.round + 1);
        }
    }

    // A tell() from inside a handler is attributed to this actor, so the
    // ponger's reply finds its way back here automatically.
    void send_ping(int round) {
        ping msg;
        msg.round = round;
        m_partner.tell(msg);
    }

    cas::actor_ref m_partner;
};

} // namespace

namespace tut {

bool run_actor_to_actor() {
    bool ok = true;

    section("Two actors, one knowing the other");
    auto ponger_ref = cas::system::create<ponger>();
    auto pinger_ref = cas::system::create<pinger>(ponger_ref);
    cas::system::start();

    section("The exchange");
    // main() is not an actor, so this message has no sender - fine, it needs
    // no reply.
    pinger_ref.tell(pinger::start{});
    ok &= check(wait_until([] { return pinger::pongs.load() == pinger::rounds; }),
                "3 rounds of ping/pong completed");
    ok &= check(ponger::pings.load() == pinger::rounds, "one ping per round");
    ok &= check(ponger::without_sender.load() == 0, "every ping had a sender to reply to");

    section("Shut down");
    cas::system::shutdown();
    cas::system::wait_for_shutdown();
    return ok;
}

} // namespace tut
