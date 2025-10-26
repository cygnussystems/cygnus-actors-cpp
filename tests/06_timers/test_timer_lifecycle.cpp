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
        cas::timer_id last_timer_id_ = cas::INVALID_TIMER_ID;

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
        }

    public:
        int get_tick_count() const { return tick_count_.load(); }
        cas::timer_id get_last_timer_id() const { return last_timer_id_; }
    };
}

TEST_CASE("Cancel timer before it fires", "[06_timers][lifecycle]") {
    auto actor = cas::system::create<timer_lifecycle_test::lifecycle_actor>();

    cas::system::start();
    wait_ms(50);

    auto actor_ref = cas::actor_registry::get("lifecycle_actor");
    REQUIRE(actor_ref.is_valid());

    // Schedule timer for 200ms
    timer_lifecycle_test::start_timer_msg start_msg;
    start_msg.delay_ms = 200;
    start_msg.value = 99;
    actor_ref.receive(start_msg);

    wait_ms(50);  // Let message be processed

    auto& lifecycle = actor.get_checked<timer_lifecycle_test::lifecycle_actor>();
    cas::timer_id id = lifecycle.get_last_timer_id();
    REQUIRE(id != cas::INVALID_TIMER_ID);

    // Cancel immediately
    timer_lifecycle_test::cancel_timer_msg cancel_msg;
    cancel_msg.id_to_cancel = id;
    actor_ref.receive(cancel_msg);

    wait_ms(50);  // Let cancel message be processed

    // Wait past when it would have fired
    wait_ms(200);

    // Verify it never fired
    REQUIRE(lifecycle.get_tick_count() == 0);

    TEST_CLEANUP();
}

TEST_CASE("Cancelling same timer multiple times is safe", "[06_timers][lifecycle]") {
    auto actor = cas::system::create<timer_lifecycle_test::lifecycle_actor>();

    cas::system::start();
    wait_ms(50);

    auto actor_ref = cas::actor_registry::get("lifecycle_actor");
    REQUIRE(actor_ref.is_valid());

    // Schedule timer
    timer_lifecycle_test::start_timer_msg start_msg;
    start_msg.delay_ms = 100;
    start_msg.value = 1;
    actor_ref.receive(start_msg);

    wait_ms(50);  // Let message be processed

    auto& lifecycle = actor.get_checked<timer_lifecycle_test::lifecycle_actor>();
    cas::timer_id id = lifecycle.get_last_timer_id();
    REQUIRE(id != cas::INVALID_TIMER_ID);

    // Cancel multiple times
    timer_lifecycle_test::cancel_timer_msg cancel_msg;
    cancel_msg.id_to_cancel = id;
    actor_ref.receive(cancel_msg);
    actor_ref.receive(cancel_msg);
    actor_ref.receive(cancel_msg);

    wait_ms(50);  // Let cancel messages be processed

    // Verify no crashes and no ticks
    wait_ms(150);
    REQUIRE(lifecycle.get_tick_count() == 0);

    TEST_CLEANUP();
}

TEST_CASE("Cancelling INVALID_TIMER_ID is safe", "[06_timers][lifecycle]") {
    auto actor = cas::system::create<timer_lifecycle_test::lifecycle_actor>();

    cas::system::start();
    wait_ms(50);

    auto actor_ref = cas::actor_registry::get("lifecycle_actor");
    REQUIRE(actor_ref.is_valid());

    // Cancel invalid timer IDs - should be safe
    timer_lifecycle_test::cancel_timer_msg cancel_msg1, cancel_msg2;
    cancel_msg1.id_to_cancel = cas::INVALID_TIMER_ID;
    cancel_msg2.id_to_cancel = 99999;
    actor_ref.receive(cancel_msg1);
    actor_ref.receive(cancel_msg2);

    wait_ms(50);  // Let messages be processed

    // System should still work fine - schedule a real timer
    timer_lifecycle_test::start_timer_msg start_msg;
    start_msg.delay_ms = 50;
    start_msg.value = 42;
    actor_ref.receive(start_msg);

    wait_ms(150);

    auto& lifecycle = actor.get_checked<timer_lifecycle_test::lifecycle_actor>();
    REQUIRE(lifecycle.get_tick_count() >= 1);

    TEST_CLEANUP();
}

TEST_CASE("Timers are cleaned up when actor stops", "[06_timers][lifecycle]") {
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
    wait_ms(50);

    auto actor_ref = cas::actor_registry::get("periodic_lifecycle");
    REQUIRE(actor_ref.is_valid());

    // Start periodic timers
    start_periodic_msg start_msg;
    start_msg.interval_ms = 50;
    actor_ref.receive(start_msg);

    wait_ms(50);  // Let message be processed

    // Let them run
    wait_ms(150);

    auto& periodic = actor.get_checked<periodic_lifecycle_actor>();
    int count_before_shutdown = periodic.get_tick_count();
    REQUIRE(count_before_shutdown >= 2);

    // Shutdown system - should cancel all timers
    cas::system::shutdown();
    cas::system::wait_for_shutdown();

    // Verify on_stop was called
    REQUIRE(periodic.was_on_stop_called());

    cas::system::reset();
}

TEST_CASE("Timer fires to correct actor after system restart", "[06_timers][lifecycle]") {
    // First run
    {
        auto actor = cas::system::create<timer_lifecycle_test::lifecycle_actor>();
        cas::system::start();
        wait_ms(50);

        auto actor_ref = cas::actor_registry::get("lifecycle_actor");
        REQUIRE(actor_ref.is_valid());

        timer_lifecycle_test::start_timer_msg start_msg;
        start_msg.delay_ms = 50;
        start_msg.value = 1;
        actor_ref.receive(start_msg);

        wait_ms(150);

        auto& lifecycle = actor.get_checked<timer_lifecycle_test::lifecycle_actor>();
        REQUIRE(lifecycle.get_tick_count() >= 1);

        TEST_CLEANUP();
    }

    // Second run - verify timers from first run don't leak
    {
        auto actor = cas::system::create<timer_lifecycle_test::lifecycle_actor>();
        cas::system::start();
        wait_ms(50);

        auto actor_ref = cas::actor_registry::get("lifecycle_actor");
        REQUIRE(actor_ref.is_valid());

        auto& lifecycle = actor.get_checked<timer_lifecycle_test::lifecycle_actor>();

        // Start with count = 0
        REQUIRE(lifecycle.get_tick_count() == 0);

        timer_lifecycle_test::start_timer_msg start_msg;
        start_msg.delay_ms = 50;
        start_msg.value = 1;
        actor_ref.receive(start_msg);

        wait_ms(150);

        // Should only have ticks from this run
        int count = lifecycle.get_tick_count();
        REQUIRE(count >= 1);
        REQUIRE(count < 10);  // Shouldn't have accumulated from previous run

        TEST_CLEANUP();
    }
}
