#include "../test_common.h"

// Timer lifecycle tests: cancellation and cleanup

namespace timer_lifecycle_test {
    struct tick : public cas::message_base {
        int value;
    };

    struct start_timer_msg : public cas::message_base {
        int delay_ms;
        int value;
    };

    struct cancel_timer_msg : public cas::message_base {
        cas::timer_id id_to_cancel;
    };

    class lifecycle_actor : public cas::actor {
    private:
        std::atomic<int> tick_count_{0};
        std::atomic<cas::timer_id> last_timer_id_{cas::INVALID_TIMER_ID};
        std::atomic<int> cancels_handled_{0};

    protected:
        void on_start() override {
            set_name("lifecycle_actor");
            handler<tick>(&lifecycle_actor::on_tick);
            handler<start_timer_msg>(&lifecycle_actor::on_start_timer_msg);
            handler<cancel_timer_msg>(&lifecycle_actor::on_cancel_timer_msg);
        }

        void on_tick(const tick&) {
            tick_count_++;
        }

        void on_start_timer_msg(const start_timer_msg& msg) {
            tick t;
            t.value = msg.value;
            last_timer_id_ = schedule_once(std::chrono::milliseconds(msg.delay_ms), t);
        }

        void on_cancel_timer_msg(const cancel_timer_msg& msg) {
            cancel_timer(msg.id_to_cancel);
            cancels_handled_++;
        }

    public:
        int get_tick_count() const { return tick_count_.load(); }
        cas::timer_id get_last_timer_id() const { return last_timer_id_; }
        int get_cancels_handled() const { return cancels_handled_.load(); }
    };
}

TEST_CASE("Cancel timer before it fires", "[06_timers][lifecycle]") {
    CAS_TEST_GUARD();
    auto actor = cas::system::create<timer_lifecycle_test::lifecycle_actor>();

    cas::system::start();
    REQUIRE(wait_until([] { return cas::actor_registry::exists("lifecycle_actor"); }));
    auto actor_ref = cas::actor_registry::get("lifecycle_actor");
    REQUIRE(actor_ref.is_valid());

    // Schedule a timer far enough out that the cancel surely comes first
    timer_lifecycle_test::start_timer_msg start_msg;
    start_msg.delay_ms = 500;
    start_msg.value = 99;
    actor_ref.tell(start_msg);

    auto& lifecycle = actor.get_checked<timer_lifecycle_test::lifecycle_actor>();
    REQUIRE(wait_until([&] { return lifecycle.get_last_timer_id() != cas::INVALID_TIMER_ID; }));
    cas::timer_id id = lifecycle.get_last_timer_id();

    // Cancel immediately
    timer_lifecycle_test::cancel_timer_msg cancel_msg;
    cancel_msg.id_to_cancel = id;
    actor_ref.tell(cancel_msg);
    REQUIRE(wait_until([&] { return lifecycle.get_cancels_handled() == 1; }));

    // Wait past when it would have fired (a real sleep: proving it does not)
    wait_ms(600);

    // Verify it never fired
    REQUIRE(lifecycle.get_tick_count() == 0);

    TEST_CLEANUP();
}

TEST_CASE("Cancelling same timer multiple times is safe", "[06_timers][lifecycle]") {
    CAS_TEST_GUARD();
    auto actor = cas::system::create<timer_lifecycle_test::lifecycle_actor>();

    cas::system::start();
    REQUIRE(wait_until([] { return cas::actor_registry::exists("lifecycle_actor"); }));
    auto actor_ref = cas::actor_registry::get("lifecycle_actor");
    REQUIRE(actor_ref.is_valid());

    // Schedule a timer far enough out that the cancels surely come first
    timer_lifecycle_test::start_timer_msg start_msg;
    start_msg.delay_ms = 500;
    start_msg.value = 1;
    actor_ref.tell(start_msg);

    auto& lifecycle = actor.get_checked<timer_lifecycle_test::lifecycle_actor>();
    REQUIRE(wait_until([&] { return lifecycle.get_last_timer_id() != cas::INVALID_TIMER_ID; }));
    cas::timer_id id = lifecycle.get_last_timer_id();

    // Cancel multiple times
    timer_lifecycle_test::cancel_timer_msg cancel_msg;
    cancel_msg.id_to_cancel = id;
    actor_ref.tell(cancel_msg);
    actor_ref.tell(cancel_msg);
    actor_ref.tell(cancel_msg);
    REQUIRE(wait_until([&] { return lifecycle.get_cancels_handled() == 3; }));

    // Verify no crashes and no ticks (a real sleep: proving it does not fire)
    wait_ms(600);
    REQUIRE(lifecycle.get_tick_count() == 0);

    TEST_CLEANUP();
}

TEST_CASE("Cancelling INVALID_TIMER_ID is safe", "[06_timers][lifecycle]") {
    CAS_TEST_GUARD();
    auto actor = cas::system::create<timer_lifecycle_test::lifecycle_actor>();

    cas::system::start();
    REQUIRE(wait_until([] { return cas::actor_registry::exists("lifecycle_actor"); }));
    auto actor_ref = cas::actor_registry::get("lifecycle_actor");
    REQUIRE(actor_ref.is_valid());

    // Cancel invalid timer IDs - should be safe
    timer_lifecycle_test::cancel_timer_msg cancel_msg1, cancel_msg2;
    cancel_msg1.id_to_cancel = cas::INVALID_TIMER_ID;
    cancel_msg2.id_to_cancel = 99999;
    actor_ref.tell(cancel_msg1);
    actor_ref.tell(cancel_msg2);

    auto& lifecycle = actor.get_checked<timer_lifecycle_test::lifecycle_actor>();
    REQUIRE(wait_until([&] { return lifecycle.get_cancels_handled() == 2; }));

    // System should still work fine - schedule a real timer
    timer_lifecycle_test::start_timer_msg start_msg;
    start_msg.delay_ms = 50;
    start_msg.value = 42;
    actor_ref.tell(start_msg);

    REQUIRE(wait_until([&] { return lifecycle.get_tick_count() >= 1; }));

    TEST_CLEANUP();
}

TEST_CASE("Timers are cleaned up when actor stops", "[06_timers][lifecycle]") {
    CAS_TEST_GUARD();
    struct start_periodic_msg : public cas::message_base {
        int interval_ms;
    };

    class periodic_lifecycle_actor : public cas::actor {
    private:
        std::atomic<int> tick_count_{0};
        bool on_stop_called_ = false;

    protected:
        void on_start() override {
            set_name("periodic_lifecycle");
            handler<timer_lifecycle_test::tick>(&periodic_lifecycle_actor::on_tick);
            handler<start_periodic_msg>(&periodic_lifecycle_actor::on_start_periodic);
        }

        void on_tick(const timer_lifecycle_test::tick&) {
            tick_count_++;
        }

        void on_start_periodic(const start_periodic_msg& msg) {
            timer_lifecycle_test::tick t;
            t.value = 1;
            schedule_periodic(std::chrono::milliseconds(msg.interval_ms), t);
        }

        void on_stop() override {
            on_stop_called_ = true;
        }

    public:
        int get_tick_count() const { return tick_count_.load(); }
        bool was_on_stop_called() const { return on_stop_called_; }
    };

    auto actor = cas::system::create<periodic_lifecycle_actor>();

    cas::system::start();
    REQUIRE(wait_until([] { return cas::actor_registry::exists("periodic_lifecycle"); }));
    auto actor_ref = cas::actor_registry::get("periodic_lifecycle");
    REQUIRE(actor_ref.is_valid());

    // Start periodic timers
    start_periodic_msg start_msg;
    start_msg.interval_ms = 50;
    actor_ref.tell(start_msg);

    auto& periodic = actor.get_checked<periodic_lifecycle_actor>();
    REQUIRE(wait_until([&] { return periodic.get_tick_count() >= 2; }));

    // Shutdown system - should cancel all timers
    cas::system::shutdown();
    cas::system::wait_for_shutdown();

    // Verify on_stop was called
    REQUIRE(periodic.was_on_stop_called());

    cas::system::reset();
}

TEST_CASE("Timer fires to correct actor after system restart", "[06_timers][lifecycle]") {
    CAS_TEST_GUARD();
    // First run
    {
        auto actor = cas::system::create<timer_lifecycle_test::lifecycle_actor>();
        cas::system::start();
        REQUIRE(wait_until([] { return cas::actor_registry::exists("lifecycle_actor"); }));
        auto actor_ref = cas::actor_registry::get("lifecycle_actor");
        REQUIRE(actor_ref.is_valid());

        timer_lifecycle_test::start_timer_msg start_msg;
        start_msg.delay_ms = 50;
        start_msg.value = 1;
        actor_ref.tell(start_msg);

        auto& lifecycle = actor.get_checked<timer_lifecycle_test::lifecycle_actor>();
        REQUIRE(wait_until([&] { return lifecycle.get_tick_count() >= 1; }));

        TEST_CLEANUP();
    }

    // Second run - verify timers from first run don't leak
    {
        auto actor = cas::system::create<timer_lifecycle_test::lifecycle_actor>();
        cas::system::start();
        REQUIRE(wait_until([] { return cas::actor_registry::exists("lifecycle_actor"); }));
        auto actor_ref = cas::actor_registry::get("lifecycle_actor");
        REQUIRE(actor_ref.is_valid());

        auto& lifecycle = actor.get_checked<timer_lifecycle_test::lifecycle_actor>();

        // Start with count = 0
        REQUIRE(lifecycle.get_tick_count() == 0);

        timer_lifecycle_test::start_timer_msg start_msg;
        start_msg.delay_ms = 50;
        start_msg.value = 1;
        actor_ref.tell(start_msg);

        // Should only have ticks from this run
        REQUIRE(wait_until([&] { return lifecycle.get_tick_count() >= 1; }));
        REQUIRE(lifecycle.get_tick_count() < 10);  // Shouldn't have accumulated from previous run

        TEST_CLEANUP();
    }
}

// Issue 013: during system shutdown an actor stops accepting messages once it
// has run on_shutdown(), so every later timer tick used to be rejected as a
// dead letter until wait_for_shutdown() stopped the timer manager. Timers are
// now cancelled right after on_shutdown(), including timers it schedules.
namespace timer_shutdown_test {
    struct tick : public cas::message_base {};

    template<typename Base>
    class ticking_actor : public Base {
    public:
        std::atomic<int> ticks{0};

    protected:
        void on_start() override {
            this->template handler<tick>(&ticking_actor::on_tick);
            this->schedule_periodic(std::chrono::milliseconds(1), tick{});
        }

        void on_tick(const tick&) {
            ticks++;
        }

        void on_shutdown() override {
            // Scheduled while stopping: cancelled along with the rest
            this->schedule_periodic(std::chrono::milliseconds(1), tick{});
        }
    };

    template<typename Base>
    void run_shutdown_with_live_timers() {
        cas::system::reset_dead_letter_stats();  // process-wide, not cleared by reset()
        auto ref = cas::system::create<ticking_actor<Base>>();
        cas::system::start();

        auto& a = ref.template get_checked<ticking_actor<Base>>();
        for (int i = 0; i < 200 && a.ticks.load() < 5; ++i) {
            wait_ms(5);
        }
        REQUIRE(a.ticks.load() >= 5);

        cas::system::shutdown();
        wait_ms(100);  // ~100 ticks would have become dead letters here
        cas::system::wait_for_shutdown();

        // Before the fix: ~100 here. A tick already on its way when the timer
        // is cancelled can still arrive (documented), so allow one per timer
        // (on_start's and on_shutdown's) - on a loaded machine that happens.
        REQUIRE(cas::system::get_dead_letter_stats().dropped_tell <= 2);
        cas::system::reset();
    }
}

TEST_CASE("System shutdown cancels timers after on_shutdown (pooled actor)", "[06_timers][lifecycle][issue013]") {
    CAS_TEST_GUARD();
    timer_shutdown_test::run_shutdown_with_live_timers<cas::actor>();
}

TEST_CASE("System shutdown cancels timers after on_shutdown (fast actor)", "[06_timers][lifecycle][issue013]") {
    CAS_TEST_GUARD();
    timer_shutdown_test::run_shutdown_with_live_timers<cas::fast_actor>();
}

TEST_CASE("A stopping actor cannot schedule timers", "[06_timers][lifecycle][issue013]") {
    CAS_TEST_GUARD();
    struct rearm : public cas::message_base {};

    // on_shutdown() still accepts messages, so its self-message is handled
    // during the drain - with the actor stopping. A timer scheduled there
    // would only produce dead letters, so none is scheduled.
    class drain_scheduler : public cas::actor {
    public:
        std::atomic<int> result{0};  // 1 = got INVALID_TIMER_ID, 2 = got a timer

    protected:
        void on_start() override {
            handler<rearm>(&drain_scheduler::on_rearm);
        }

        void on_shutdown() override {
            self().tell(rearm{});
        }

        void on_rearm(const rearm&) {
            auto id = schedule_periodic(std::chrono::milliseconds(1), timer_shutdown_test::tick{});
            result = (id == cas::INVALID_TIMER_ID) ? 1 : 2;
        }
    };

    cas::system::reset_dead_letter_stats();
    auto ref = cas::system::create<drain_scheduler>();
    cas::system::start();
    auto& a = ref.get_checked<drain_scheduler>();

    cas::system::shutdown();
    wait_ms(50);
    cas::system::wait_for_shutdown();

    REQUIRE(a.result.load() == 1);
    REQUIRE(cas::system::get_dead_letter_stats().dropped_tell == 0);
    cas::system::reset();
}
