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

// The messages. Timer messages are ordinary messages; they simply arrive later.
namespace msg {

// Sent by main(). schedule_*() are protected actor members - an actor arms
// timers on itself - so the outside world asks it to by message. (Arming in
// on_start() is just as common.)
struct arm : cas::message_base {};

// Only the ticker sends these, to itself, through its timers.
struct one_shot : cas::message_base {
    int label = 0;
};

struct heartbeat : cas::message_base {};

} // namespace msg

// The actors.
namespace actor {

class ticker : public cas::actor {
public:
    static constexpr int s_heartbeat_limit = 3;
    static inline std::atomic<int> s_one_shot_count{0};      // observed by the tutorial
    static inline std::atomic<int> s_heartbeat_count{0};
    static inline std::atomic<bool> s_cancelled{false};

protected:
    void on_start() override {
        set_name("ticker");
        handler<msg::arm>(&ticker::on_arm);
        handler<msg::one_shot>(&ticker::on_one_shot);
        handler<msg::heartbeat>(&ticker::on_heartbeat);
    }

private:
    void on_arm(const msg::arm&) {
        // Delivered once after the delay. The message is copied, so a local
        // is fine.
        msg::one_shot once;
        once.label = 1;
        schedule_once(std::chrono::milliseconds(30), once);

        // Redelivered every interval until cancelled - keep the id.
        m_heartbeat = schedule_periodic(std::chrono::milliseconds(10), msg::heartbeat{});
        std::cout << "  timers armed\n";
    }

    void on_one_shot(const msg::one_shot& _msg) {
        std::cout << "  one-shot timer fired (label " << _msg.label << ")\n";
        s_one_shot_count.fetch_add(1);
    }

    void on_heartbeat(const msg::heartbeat&) {
        const int count = s_heartbeat_count.fetch_add(1) + 1;
        std::cout << "  heartbeat " << count << "\n";

        // Cancelling from the timer's own handler is the normal way to end a
        // periodic timer. Cancelling a dead id is harmless.
        if (count >= s_heartbeat_limit) {
            cancel_timer(m_heartbeat);
            s_cancelled.store(true);
            std::cout << "  heartbeat cancelled after " << s_heartbeat_limit << "\n";
        }
    }

    cas::timer_id m_heartbeat = cas::INVALID_TIMER_ID;
};

} // namespace actor

} // namespace

namespace tut {

bool run_timers() {
    bool ok = true;

    heading("Arming timers");
    //-----------------------
    auto ticker = cas::system::create<actor::ticker>();
    cas::system::start();
    ticker.tell(msg::arm{});

    ok &= check(wait_until(actor::ticker::s_one_shot_count, 1),
                "one-shot timer fired exactly once");
    ok &= check(wait_until(actor::ticker::s_cancelled),
                "periodic timer fired repeatedly, then cancelled itself");

    heading("Confirming the cancellation held");
    //------------------------------------------
    // Several more intervals pass; nothing may move.
    const int after_cancel = actor::ticker::s_heartbeat_count.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    ok &= check(actor::ticker::s_heartbeat_count.load() == after_cancel, "no further heartbeats after cancel_timer()");
    ok &= check(actor::ticker::s_one_shot_count.load() == 1, "the one-shot timer did not repeat");

    heading("Shutting down");
    //-----------------------
    cas::system::shutdown();
    cas::system::wait_for_shutdown();
    return ok;
}

} // namespace tut
