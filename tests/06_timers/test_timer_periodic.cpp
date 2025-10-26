#include "../test_common.h"

// Periodic timer tests: timers that fire repeatedly

namespace timer_periodic_test {
    struct tick : public cas::message_base {
        int value;
    };

    struct start_periodic : public cas::message_base {
        int interval_ms;
        int value;
    };

    struct stop_periodic : public cas::message_base {};

    class periodic_actor : public cas::actor {
    private:
        std::atomic<int> tick_count_{0};
        std::atomic<int> total_value_{0};
        cas::timer_id periodic_timer_ = cas::INVALID_TIMER_ID;

    protected:
        void on_start() override {
            set_name("periodic_actor");
            handler<tick>(&periodic_actor::on_tick);
            handler<start_periodic>(&periodic_actor::on_start_periodic);
            handler<stop_periodic>(&periodic_actor::on_stop_periodic);
        }

        void on_tick(const tick& msg) {
            tick_count_++;
            total_value_ += msg.value;
        }

        void on_start_periodic(const start_periodic& msg) {
            tick t;
            t.value = msg.value;
            periodic_timer_ = schedule_periodic(std::chrono::milliseconds(msg.interval_ms), t);
        }

        void on_stop_periodic(const stop_periodic&) {
            if (periodic_timer_ != cas::INVALID_TIMER_ID) {
                cancel_timer(periodic_timer_);
                periodic_timer_ = cas::INVALID_TIMER_ID;
            }
        }

    public:
        int get_tick_count() const { return tick_count_.load(); }
        int get_total_value() const { return total_value_.load(); }
        cas::timer_id get_timer_id() const { return periodic_timer_; }
    };
}

TEST_CASE("Periodic timer fires repeatedly", "[06_timers][periodic]") {
    auto actor = cas::system::create<timer_periodic_test::periodic_actor>();

    cas::system::start();
    wait_ms(50);

    auto actor_ref = cas::actor_registry::get("periodic_actor");
    REQUIRE(actor_ref.is_valid());

    // Start periodic timer with 50ms interval
    timer_periodic_test::start_periodic start_msg;
    start_msg.interval_ms = 50;
    start_msg.value = 10;
    actor_ref.receive(start_msg);

    wait_ms(50);  // Let message be processed

    auto& periodic = actor.get_checked<timer_periodic_test::periodic_actor>();
    REQUIRE(periodic.get_timer_id() != cas::INVALID_TIMER_ID);

    // After ~75ms, should have fired at least once
    wait_ms(50);
    int count1 = periodic.get_tick_count();
    REQUIRE(count1 >= 1);

    // After ~175ms total, should have fired at least 3 times
    wait_ms(100);
    int count2 = periodic.get_tick_count();
    REQUIRE(count2 >= 3);
    REQUIRE(count2 > count1);

    // After ~275ms total, should have fired at least 5 times
    wait_ms(100);
    int count3 = periodic.get_tick_count();
    REQUIRE(count3 >= 5);
    REQUIRE(count3 > count2);

    // Stop timer
    timer_periodic_test::stop_periodic stop_msg;
    actor_ref.receive(stop_msg);

    wait_ms(50);  // Let stop message be processed

    // Wait and verify no more ticks
    int count_after_stop = periodic.get_tick_count();
    wait_ms(150);
    int final_count = periodic.get_tick_count();
    REQUIRE(final_count == count_after_stop);  // No new ticks after cancellation

    TEST_CLEANUP();
}

TEST_CASE("Periodic timer copies message for each firing", "[06_timers][periodic]") {
    auto actor = cas::system::create<timer_periodic_test::periodic_actor>();

    cas::system::start();
    wait_ms(50);

    auto actor_ref = cas::actor_registry::get("periodic_actor");
    REQUIRE(actor_ref.is_valid());

    // Start periodic timer that sends value=5 each time
    timer_periodic_test::start_periodic start_msg;
    start_msg.interval_ms = 50;
    start_msg.value = 5;
    actor_ref.receive(start_msg);

    wait_ms(50);  // Let message be processed

    // Wait for multiple firings
    wait_ms(250);

    auto& periodic = actor.get_checked<timer_periodic_test::periodic_actor>();

    // Each tick should add 5 to total_value
    int tick_count = periodic.get_tick_count();
    int total_value = periodic.get_total_value();

    REQUIRE(tick_count >= 4);  // At least 4 ticks in 250ms with 50ms interval
    REQUIRE(total_value == tick_count * 5);  // Verify each message had value=5

    TEST_CLEANUP();
}

TEST_CASE("Multiple periodic timers run independently", "[06_timers][periodic]") {
    struct fast_tick : public cas::message_base {};
    struct slow_tick : public cas::message_base {};

    class multi_timer_actor : public cas::actor {
    private:
        std::atomic<int> fast_ticks_{0};
        std::atomic<int> slow_ticks_{0};

    protected:
        void on_start() override {
            set_name("multi_timer");
            handler<fast_tick>(&multi_timer_actor::on_fast_tick);
            handler<slow_tick>(&multi_timer_actor::on_slow_tick);

            // Fast timer: 30ms interval
            fast_tick f_msg;
            schedule_periodic(std::chrono::milliseconds(30), f_msg);

            // Slow timer: 100ms interval
            slow_tick s_msg;
            schedule_periodic(std::chrono::milliseconds(100), s_msg);
        }

        void on_fast_tick(const fast_tick&) {
            fast_ticks_++;
        }

        void on_slow_tick(const slow_tick&) {
            slow_ticks_++;
        }

    public:
        int get_fast_ticks() const { return fast_ticks_.load(); }
        int get_slow_ticks() const { return slow_ticks_.load(); }
    };

    auto actor = cas::system::create<multi_timer_actor>();

    cas::system::start();
    wait_ms(50);

    auto& multi = actor.get_checked<multi_timer_actor>();

    // Wait 350ms
    wait_ms(350);

    int fast_count = multi.get_fast_ticks();
    int slow_count = multi.get_slow_ticks();

    // Fast timer (30ms) should fire ~11 times in 350ms
    // Slow timer (100ms) should fire ~3 times in 350ms
    REQUIRE(fast_count >= 10);
    REQUIRE(slow_count >= 3);
    REQUIRE(fast_count > slow_count * 2);  // Fast should be significantly more

    TEST_CLEANUP();
}

TEST_CASE("Cancelled periodic timer stops firing", "[06_timers][periodic]") {
    auto actor = cas::system::create<timer_periodic_test::periodic_actor>();

    cas::system::start();
    wait_ms(50);

    auto actor_ref = cas::actor_registry::get("periodic_actor");
    REQUIRE(actor_ref.is_valid());

    // Start periodic timer
    timer_periodic_test::start_periodic start_msg;
    start_msg.interval_ms = 50;
    start_msg.value = 1;
    actor_ref.receive(start_msg);

    wait_ms(50);  // Let message be processed

    // Let it fire a few times
    wait_ms(150);

    auto& periodic = actor.get_checked<timer_periodic_test::periodic_actor>();
    int count_before_cancel = periodic.get_tick_count();
    REQUIRE(count_before_cancel >= 2);

    // Cancel timer
    timer_periodic_test::stop_periodic stop_msg;
    actor_ref.receive(stop_msg);

    wait_ms(50);  // Let stop message be processed

    // Wait and verify it stopped
    int count_after_cancel = periodic.get_tick_count();
    wait_ms(200);
    int final_count = periodic.get_tick_count();
    REQUIRE(final_count == count_after_cancel);

    TEST_CLEANUP();
}
