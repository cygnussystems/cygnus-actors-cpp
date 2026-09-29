// Tutorial 7: Timers
//
// TEACHES
//   - schedule_once() for a delayed message
//   - schedule_periodic() for a repeating one, and cancel_timer() to stop it
//   - why timers are armed from inside the actor, not from main()
//
// ASSUMES
//   Tutorials 1-6.
//
// THE IDEA
//   A timer runs no callback on another thread: it sends the actor a message
//   when it fires. That message is handled on the actor's own thread, in turn
//   with everything else, so timer code needs no more care than any handler.

#include "tutorials.h"
#include "cas/cas.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

namespace {

class ticker : public cas::actor {
public:
    // Sent by main(). schedule_*() are protected actor members - an actor
    // arms timers on itself - so the outside world asks it to by message.
    // (Arming in on_start() is just as common.)
    struct arm : cas::message_base {};

    static constexpr int heartbeat_limit = 3;
    static inline std::atomic<int> one_shots{0};        // observed by the tutorial
    static inline std::atomic<int> heartbeats{0};
    static inline std::atomic<bool> cancelled{false};

protected:
    void on_start() override {
        set_name("ticker");
        handler<arm>(&ticker::on_arm);
        handler<one_shot>(&ticker::on_one_shot);
        handler<heartbeat>(&ticker::on_heartbeat);
    }

private:
    // Timer messages are ordinary messages; they simply arrive later. Only
    // the ticker sends them to itself, so they are private.
    struct one_shot : cas::message_base {
        int label = 0;
    };
    struct heartbeat : cas::message_base {};

    void on_arm(const arm&) {
        // Delivered once after the delay. The message is copied, so a local
        // is fine.
        one_shot msg;
        msg.label = 1;
        schedule_once(std::chrono::milliseconds(30), msg);

        // Redelivered every interval until cancelled - keep the id.
        m_heartbeat = schedule_periodic(std::chrono::milliseconds(10), heartbeat{});
        std::cout << "  timers armed\n";
    }

    void on_one_shot(const one_shot& msg) {
        std::cout << "  one-shot timer fired (label " << msg.label << ")\n";
        one_shots.fetch_add(1);
    }

    void on_heartbeat(const heartbeat&) {
        const int count = heartbeats.fetch_add(1) + 1;
        std::cout << "  heartbeat " << count << "\n";

        // Cancelling from the timer's own handler is the normal way to end a
        // periodic timer. Cancelling a dead id is harmless.
        if (count >= heartbeat_limit) {
            cancel_timer(m_heartbeat);
            cancelled.store(true);
            std::cout << "  heartbeat cancelled after " << heartbeat_limit << "\n";
        }
    }

    cas::timer_id m_heartbeat = cas::INVALID_TIMER_ID;
};

} // namespace

namespace tut {

bool run_timers() {
    bool ok = true;

    section("Arming timers");
    auto ticker_ref = cas::system::create<ticker>();
    cas::system::start();
    ticker_ref.tell(ticker::arm{});

    ok &= check(wait_until([] { return ticker::one_shots.load() == 1; }),
                "one-shot timer fired exactly once");
    ok &= check(wait_until([] { return ticker::cancelled.load(); }),
                "periodic timer fired repeatedly, then cancelled itself");

    section("Confirming the cancellation held");
    // Several more intervals pass; nothing may move.
    const int after_cancel = ticker::heartbeats.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    ok &= check(ticker::heartbeats.load() == after_cancel, "no further heartbeats after cancel_timer()");
    ok &= check(ticker::one_shots.load() == 1, "the one-shot timer did not repeat");

    section("Shutting down");
    cas::system::shutdown();
    cas::system::wait_for_shutdown();
    return ok;
}

} // namespace tut
