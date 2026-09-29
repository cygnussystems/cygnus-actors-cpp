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

// The messages. A request and its reply are ordinary messages that travel in
// opposite directions; nothing pairs them but their names.
namespace msg {

struct start : cas::message_base {};  // from main(): begin the exchange

struct ping : cas::message_base {
    int round = 0;
};

struct pong : cas::message_base {
    int round = 0;
};

} // namespace msg

// The actors.
namespace actor {

// Answers every ping to whoever sent it. It never needs to know its partner.
class ponger : public cas::actor {
public:
    static inline std::atomic<int> s_ping_count{0};            // observed by the tutorial
    static inline std::atomic<int> s_without_sender_count{0};

protected:
    void on_start() override {
        set_name("ponger");
        handler<msg::ping>(&ponger::on_ping);
    }

private:
    void on_ping(const msg::ping& _msg) {
        std::cout << "  ponger <- ping " << _msg.round << "\n";
        s_ping_count.fetch_add(1);

        // _msg.sender is set only when an actor sent the message; a tell()
        // from main() has none. Check before replying.
        if (!_msg.sender.is_valid()) {
            s_without_sender_count.fetch_add(1);
            return;
        }
        msg::pong reply;
        reply.round = _msg.round;
        _msg.sender.tell(reply);
    }
};

// Starts the exchange and stops it after s_target_rounds - two actors answering each
// other forever would keep a thread busy for good.
class pinger : public cas::actor {
public:
    static constexpr int s_target_rounds = 3;
    static inline std::atomic<int> s_pong_count{0};             // observed by the tutorial

    // Arguments to create<pinger>(...) are forwarded here: this is how an
    // actor gets to know its collaborators.
    explicit pinger(cas::actor_ref partner) : m_partner(std::move(partner)) {}

protected:
    void on_start() override {
        set_name("pinger");
        handler<msg::start>(&pinger::on_begin);
        handler<msg::pong>(&pinger::on_pong);
    }

private:
    void on_begin(const msg::start&) {
        send_ping(1);
    }

    void on_pong(const msg::pong& _msg) {
        std::cout << "  pinger <- pong " << _msg.round << "\n";
        s_pong_count.fetch_add(1);
        if (_msg.round < s_target_rounds) {
            send_ping(_msg.round + 1);
        }
    }

    // A tell() from inside a handler is attributed to this actor, so the
    // ponger's reply finds its way back here automatically.
    void send_ping(int round) {
        msg::ping out;
        out.round = round;
        m_partner.tell(out);
    }

    cas::actor_ref m_partner;
};

} // namespace actor

} // namespace

namespace tut {

bool run_actor_to_actor() {
    bool ok = true;

    heading("Two actors, one knowing the other");
    //-------------------------------------------
    auto ponger_ref = cas::system::create<actor::ponger>();
    auto pinger_ref = cas::system::create<actor::pinger>(ponger_ref);
    cas::system::start();

    heading("The exchange");
    //----------------------
    // main() is not an actor, so this message has no sender - fine, it needs
    // no reply.
    pinger_ref.tell(msg::start{});
    ok &= check(wait_until(actor::pinger::s_pong_count, actor::pinger::s_target_rounds), "3 rounds of ping/pong completed");
    ok &= check(actor::ponger::s_ping_count.load() == actor::pinger::s_target_rounds, "one ping per round");
    ok &= check(actor::ponger::s_without_sender_count.load() == 0, "every ping had a sender to reply to");

    heading("Shut down");
    //-------------------
    cas::system::shutdown();
    cas::system::wait_for_shutdown();
    return ok;
}

} // namespace tut
