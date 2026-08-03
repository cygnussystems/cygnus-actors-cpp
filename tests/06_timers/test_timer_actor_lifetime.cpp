#include "../test_common.h"

// Timer callbacks must not outlive the actor they deliver to.
//
// The timer thread copies a callback out of the map under the timer mutex,
// releases the mutex, and only then invokes it. Cancellation cannot reach a
// callback already in that window: cancel() marks the timer and erases the
// stored callback, but the extracted copy is beyond its reach.
//
// stop_actor() cancels an actor's timers and then removes the actor, without
// waiting for an extracted callback to finish. So an in-flight callback can
// run after its target actor has been destroyed. When the callback captured
// the actor as a raw pointer, that was a use-after-free.
//
// These tests hammer the window - schedule timers with short intervals, then
// stop or shut down while they are firing. A regression shows up as a crash
// or a sanitizer report rather than a failed assertion, so reaching the end
// of the test is the result being checked.

namespace timer_lifetime_test {

    struct tick : public cas::message_base {
        int value = 0;
    };

    struct arm : public cas::message_base {
        int interval_ms = 1;
    };

    std::atomic<int> g_ticks{0};

    class timer_owner : public cas::actor {
    protected:
        void on_start() override {
            handler<tick>(&timer_owner::on_tick);
            handler<arm>(&timer_owner::on_arm);
        }

        void on_arm(const arm& msg) {
            // A fast periodic timer maximises the chance of a callback being
            // in flight when the actor goes away.
            tick t;
            t.value = 1;
            schedule_periodic(std::chrono::milliseconds(msg.interval_ms), t);
        }

        void on_tick(const tick&) {
            g_ticks.fetch_add(1);
        }
    };

} // namespace timer_lifetime_test

TEST_CASE("Stopping an actor with timers firing does not use freed memory",
          "[06_timers][lifetime]") {
    CAS_TEST_GUARD();
    using namespace timer_lifetime_test;

    g_ticks.store(0);

    cas::system::start();

    // Repeat: the window between extracting a callback and invoking it is
    // small, so a single attempt proves little.
    for (int round = 0; round < 25; ++round) {
        // The actor_ref must go out of scope for the actor to be destroyed.
        // stop_actor() removes the system's shared_ptr, but a caller holding
        // its own ref keeps the object alive - so a test that keeps the ref
        // never reaches the dangling case at all, whatever the callback
        // captured.
        {
            auto owner = cas::system::create<timer_owner>();

            arm a;
            a.interval_ms = 1;
            owner.tell(a);

            // Let the timer actually start firing before pulling the actor
            // out from under it.
            wait_ms(5);

            cas::system::stop_actor(owner);
        }  // last shared_ptr released here - the actor is destroyed

        // Give any callback that was already extracted a chance to run
        // against the freed actor.
        wait_ms(2);
    }

    // Surviving the loop is the point. The assertion just pins that timers
    // really did fire, so the test is not silently exercising nothing.
    REQUIRE(g_ticks.load() > 0);
}

TEST_CASE("System shutdown with timers firing does not use freed memory",
          "[06_timers][lifetime]") {
    CAS_TEST_GUARD();
    using namespace timer_lifetime_test;

    g_ticks.store(0);

    cas::system::start();

    for (int i = 0; i < 8; ++i) {
        auto owner = cas::system::create<timer_owner>();
        arm a;
        a.interval_ms = 1;
        owner.tell(a);
    }

    wait_ms(20);

    // Whole-system shutdown stops the timer manager before clearing actors,
    // so this path is better protected than individual removal - but it is
    // worth pinning that it stays that way.
    cas::system::shutdown();
    cas::system::wait_for_shutdown();

    REQUIRE(g_ticks.load() > 0);
}

TEST_CASE("Timers firing during a discard-mode stop do not use freed memory",
          "[06_timers][lifetime]") {
    CAS_TEST_GUARD();
    using namespace timer_lifetime_test;

    g_ticks.store(0);
    cas::system::start();

    // discard + async is the narrowest path through stop_actor(): it skips
    // the drain wait, so far less time passes between cancelling the timers
    // and the actor being released. That makes it the most likely to catch a
    // callback still in flight.
    cas::stop_config cfg;
    cfg.mode = cas::stop_mode::discard;
    cfg.wait_for_stop = false;

    for (int round = 0; round < 40; ++round) {
        {
            auto owner = cas::system::create<timer_owner>();
            arm a;
            a.interval_ms = 1;
            owner.tell(a);
            wait_ms(3);
            cas::system::stop_actor(owner, cfg);
        }
        wait_ms(1);
    }

    // Deliberately no assertion on g_ticks here.
    //
    // This case races on purpose: with a 1ms timer, a 3ms window and an async
    // discard stop, a round may legitimately tear the actor down before any
    // timer fires. Requiring a tick made the test fail about once in twenty
    // runs for a reason that had nothing to do with the behaviour under test.
    //
    // Reaching this line without a crash or a sanitizer report IS the result.
    // The preceding test case already pins that timers fire at all.
    SUCCEED("survived 40 discard-mode stops with timers in flight");
}

// A NOTE ON WHAT THESE TESTS DO AND DO NOT GUARANTEE
//
// They exercise the right window, but they catch a reintroduction only by
// winning a narrow race through repetition, and measurement says that is
// unreliable: with the weak_ptr capture reverted to a raw pointer, ASan
// caught the use-after-free on one occasion but then 0 times in 15 further
// runs. Treat them as opportunistic, not as a gate.
//
// Making it deterministic needs a pause between the timer thread copying the
// callback out of m_callbacks and invoking it (timer_manager.cpp, around the
// lock.unlock() before callback(...)). That window is inside timer_manager
// and cannot be widened from a test. Parking inside the actor's *handler*
// does not work - the unsafe read is actor::enqueue_message() reading
// m_state, which happens on the timer thread before any handler runs.
//
// Doing it properly means a test seam in timer_manager - something like an
// injectable hook invoked between extraction and invocation, compiled out of
// release builds. That is a deliberate design change to production code, so
// it is left as a follow-up rather than smuggled in with the fix.

TEST_CASE("A timer message for a stopped actor is dropped, not delivered",
          "[06_timers][lifetime]") {
    CAS_TEST_GUARD();
    using namespace timer_lifetime_test;

    g_ticks.store(0);

    cas::system::start();

    auto owner = cas::system::create<timer_owner>();
    arm a;
    a.interval_ms = 5;
    owner.tell(a);

    wait_ms(30);
    const int before_stop = g_ticks.load();
    REQUIRE(before_stop > 0);  // the timer was running

    cas::system::stop_actor(owner);

    // Once the actor is stopped, its periodic timer must stop delivering.
    // Give it several intervals to prove the count has really stopped moving.
    const int after_stop = g_ticks.load();
    wait_ms(50);

    REQUIRE(g_ticks.load() == after_stop);
}
