// Tutorial 7: Timers
//
// TEACHES
//   - schedule_once() for a delayed message
//   - schedule_periodic() for a repeating one
//   - cancel_timer(), and why a periodic timer needs cancelling
//   - why timers are scheduled from inside a handler, not from main()
//
// ASSUMES
//   Tutorials 1-6.
//
// THE IDEA
//   A timer does not run a callback on some other thread. It sends the actor
//   a message when it fires. That is the whole model, and it is what keeps
//   timers safe: the timer message is handled on the actor's own thread, in
//   turn with everything else, so timer code needs no more care than a
//   handler does.

#include "tutorials.h"
#include "cas/cas.h"

#include <atomic>
#include <chrono>
#include <iostream>

namespace {

// Timer messages are ordinary messages. Nothing marks them as timer-related;
// they simply arrive later.
struct one_shot_fired : cas::message_base {
    int label = 0;
};

struct heartbeat : cas::message_base {};

// Tells the actor to set its timers up. See the note in run() for why this is
// a message rather than something main() does directly.
struct arm_timers : cas::message_base {};

std::atomic<int> g_one_shots{0};
std::atomic<int> g_heartbeats{0};
std::atomic<bool> g_cancelled{false};

class ticker : public cas::actor {
protected:
    void on_start() override {
        set_name("ticker");
        handler<arm_timers>(&ticker::on_arm_timers);
        handler<one_shot_fired>(&ticker::on_one_shot_fired);
        handler<heartbeat>(&ticker::on_heartbeat);
    }

    void on_arm_timers(const arm_timers&) {
        // --- 1. A one-shot timer --------------------------------------------
        //
        // Delivers the message once, after the delay, then forgets about it.
        // The message is passed by value and copied, so it may be a local.
        one_shot_fired msg;
        msg.label = 1;
        schedule_once(std::chrono::milliseconds(30), msg);

        // --- 2. A periodic timer --------------------------------------------
        //
        // Redelivers every interval until cancelled. Keep the id: a periodic
        // timer that nobody cancels keeps firing for the lifetime of the
        // actor, which is rarely what you want.
        m_heartbeat_id = schedule_periodic(std::chrono::milliseconds(10),
                                           heartbeat{});

        std::cout << "  timers armed\n";
    }

    void on_one_shot_fired(const one_shot_fired& msg) {
        std::cout << "  one-shot timer fired (label " << msg.label << ")\n";
        g_one_shots.fetch_add(1);
    }

    void on_heartbeat(const heartbeat&) {
        const int count = g_heartbeats.fetch_add(1) + 1;
        std::cout << "  heartbeat " << count << "\n";

        // --- 3. Cancelling ---------------------------------------------------
        //
        // Cancelling from inside the timer's own handler is a normal way to
        // stop a periodic timer once its work is done.
        //
        // cancel_timer() on an id that has already fired or been cancelled is
        // harmless, so there is no need to track whether it is still live.
        if (count >= 3) {
            cancel_timer(m_heartbeat_id);
            g_cancelled.store(true);
            std::cout << "  heartbeat cancelled after 3\n";
        }
    }

private:
    cas::timer_id m_heartbeat_id = cas::INVALID_TIMER_ID;
};

} // namespace

namespace tut {

bool run_timers() {
    bool ok = true;

    section("Starting");
    auto ticker_ref = cas::system::create<ticker>();
    cas::system::start();

    // --- 4. Why arming happens in a handler ----------------------------------
    //
    // schedule_once() and schedule_periodic() are protected members of actor:
    // they are called by an actor, on itself, and the timer message is
    // delivered back to that actor. main() is not an actor and has no such
    // method to call.
    //
    // So to start a timer from outside, send the actor a message and let it
    // arm its own timers. (Arming them directly in on_start() is also fine
    // and very common - this tutorial uses a message only to make the
    // sequence visible.)

    section("Arming timers");
    ticker_ref.tell(arm_timers{});

    ok &= check(wait_until([] { return g_one_shots.load() == 1; }),
                "one-shot timer fired exactly once");

    ok &= check(wait_until([] { return g_cancelled.load(); }),
                "periodic timer fired repeatedly, then cancelled itself");

    // --- 5. Cancellation really stops it -------------------------------------
    //
    // Give it several more intervals; the count must not move.

    section("Confirming the cancellation held");
    const int after_cancel = g_heartbeats.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    ok &= check(g_heartbeats.load() == after_cancel,
                "no further heartbeats after cancel_timer()");

    ok &= check(g_one_shots.load() == 1,
                "the one-shot timer did not repeat");

    section("Shutting down");
    cas::system::shutdown();
    cas::system::wait_for_shutdown();

    return ok;
}

} // namespace tut
