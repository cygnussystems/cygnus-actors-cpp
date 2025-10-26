#include "../test_common.h"

// Basic timer tests: one-shot timers

namespace timer_basic_test {
    struct tick : public cas::message_base {
        int value;
    };

    struct start_timer : public cas::message_base {
        int delay_ms;
        int value;
    };

    class timer_actor : public cas::actor {
    private:
        std::atomic<int> tick_count_{0};
        int last_value_ = 0;
        cas::timer_id last_timer_id_ = cas::INVALID_TIMER_ID;

    protected:
        void on_start() override {
            set_name("timer_actor");
            handler<tick>(&timer_actor::on_tick);
            handler<start_timer>(&timer_actor::on_start_timer);
        }

        void on_tick(const tick& msg) {
            tick_count_++;
            last_value_ = msg.value;
        }

        void on_start_timer(const start_timer& msg) {
            tick t;
            t.value = msg.value;
            last_timer_id_ = schedule_once(std::chrono::milliseconds(msg.delay_ms), t);
        }

    public:
        int get_tick_count() const { return tick_count_.load(); }
        int get_last_value() const { return last_value_; }
        cas::timer_id get_last_timer_id() const { return last_timer_id_; }
    };
}

TEST_CASE("One-shot timer fires after delay", "[06_timers][basic]") {
    auto actor = cas::system::create<timer_basic_test::timer_actor>();

    cas::system::start();
    wait_ms(50);

    auto actor_ref = cas::actor_registry::get("timer_actor");
    REQUIRE(actor_ref.is_valid());

    // Tell actor to schedule a timer for 100ms
    timer_basic_test::start_timer start_msg;
    start_msg.delay_ms = 100;
    start_msg.value = 42;
    actor_ref.receive(start_msg);

    wait_ms(50);  // Let message be processed and timer scheduled

    // Verify timer was scheduled
    auto& timer_actor = actor.get_checked<timer_basic_test::timer_actor>();
    REQUIRE(timer_actor.get_last_timer_id() != cas::INVALID_TIMER_ID);

    // Wait less than delay - should not have fired yet
    wait_ms(30);
    REQUIRE(timer_actor.get_tick_count() == 0);

    // Wait for timer to fire
    wait_ms(50);
    REQUIRE(timer_actor.get_tick_count() == 1);
    REQUIRE(timer_actor.get_last_value() == 42);

    // Wait more - should not fire again (one-shot)
    wait_ms(200);
    REQUIRE(timer_actor.get_tick_count() == 1);

    TEST_CLEANUP();
}

TEST_CASE("Multiple one-shot timers fire independently", "[06_timers][basic]") {
    auto actor = cas::system::create<timer_basic_test::timer_actor>();

    cas::system::start();
    wait_ms(50);

    auto actor_ref = cas::actor_registry::get("timer_actor");
    REQUIRE(actor_ref.is_valid());

    // Schedule 3 timers with different delays
    timer_basic_test::start_timer msg1, msg2, msg3;
    msg1.delay_ms = 50;
    msg1.value = 1;
    msg2.delay_ms = 100;
    msg2.value = 2;
    msg3.delay_ms = 150;
    msg3.value = 3;

    actor_ref.receive(msg1);
    actor_ref.receive(msg2);
    actor_ref.receive(msg3);

    wait_ms(50);  // Let messages be processed

    auto& timer_actor = actor.get_checked<timer_basic_test::timer_actor>();

    // After 75ms, first should have fired
    wait_ms(40);
    REQUIRE(timer_actor.get_tick_count() == 1);

    // After 125ms total, first two should have fired
    wait_ms(50);
    REQUIRE(timer_actor.get_tick_count() == 2);

    // After 175ms total, all three should have fired
    wait_ms(50);
    REQUIRE(timer_actor.get_tick_count() == 3);
    REQUIRE(timer_actor.get_last_value() == 3);

    TEST_CLEANUP();
}

TEST_CASE("Timer with zero delay fires immediately", "[06_timers][basic]") {
    auto actor = cas::system::create<timer_basic_test::timer_actor>();

    cas::system::start();
    wait_ms(50);

    auto actor_ref = cas::actor_registry::get("timer_actor");
    REQUIRE(actor_ref.is_valid());

    timer_basic_test::start_timer start_msg;
    start_msg.delay_ms = 0;
    start_msg.value = 99;
    actor_ref.receive(start_msg);

    // Should fire almost immediately
    wait_ms(100);

    auto& timer_actor = actor.get_checked<timer_basic_test::timer_actor>();
    REQUIRE(timer_actor.get_tick_count() == 1);
    REQUIRE(timer_actor.get_last_value() == 99);

    TEST_CLEANUP();
}
