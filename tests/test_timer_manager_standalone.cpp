#include "../include/catch.hpp"
#include "cas/timer_manager.h"
#include "cas/message_base.h"
#include <atomic>
#include <thread>
#include <chrono>

// Simple test message
struct test_tick : public cas::message_base {
    int value = 0;
};

// Helper to wait (static to avoid linker conflicts)
static void wait_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

TEST_CASE("Timer manager can start and stop", "[timer_manager][standalone]") {
    cas::timer_manager mgr;

    REQUIRE(!mgr.is_running());

    mgr.start();
    REQUIRE(mgr.is_running());

    mgr.stop();
    REQUIRE(!mgr.is_running());
}

TEST_CASE("Timer manager fires one-shot timer", "[timer_manager][standalone]") {
    cas::timer_manager mgr;
    mgr.start();

    std::atomic<int> fire_count{0};
    std::atomic<int> received_value{0};

    // Create message
    auto msg = std::make_unique<test_tick>();
    msg->value = 42;

    // Create copy function
    auto copier = [msg_val = msg->value]() -> std::unique_ptr<cas::message_base> {
        auto copy = std::make_unique<test_tick>();
        copy->value = msg_val;
        return copy;
    };

    // Schedule timer
    auto id = mgr.schedule(
        std::move(msg),
        std::move(copier),
        std::chrono::milliseconds(50),
        std::chrono::milliseconds(0),  // One-shot
        [&](cas::timer_id, std::unique_ptr<cas::message_base> fired_msg) {
            fire_count++;
            auto* tick = static_cast<test_tick*>(fired_msg.get());
            received_value = tick->value;
        }
    );

    REQUIRE(id != cas::INVALID_TIMER_ID);
    REQUIRE(mgr.active_count() == 1);

    // Wait for timer to fire
    wait_ms(150);

    REQUIRE(fire_count == 1);
    REQUIRE(received_value == 42);
    REQUIRE(mgr.active_count() == 0);  // One-shot should be removed

    mgr.stop();
}

TEST_CASE("Timer manager fires periodic timer multiple times", "[timer_manager][standalone]") {
    cas::timer_manager mgr;
    mgr.start();

    std::atomic<int> fire_count{0};

    // Create message
    auto msg = std::make_unique<test_tick>();
    msg->value = 99;

    // Create copy function
    auto copier = [msg_val = msg->value]() -> std::unique_ptr<cas::message_base> {
        auto copy = std::make_unique<test_tick>();
        copy->value = msg_val;
        return copy;
    };

    // Schedule periodic timer (fires every 30ms)
    auto id = mgr.schedule(
        std::move(msg),
        std::move(copier),
        std::chrono::milliseconds(30),
        std::chrono::milliseconds(30),  // Periodic
        [&](cas::timer_id, std::unique_ptr<cas::message_base>) {
            fire_count++;
        }
    );

    REQUIRE(id != cas::INVALID_TIMER_ID);

    // Wait for ~3 firings
    wait_ms(120);

    int count = fire_count.load();
    REQUIRE(count >= 3);
    REQUIRE(count <= 5);  // Allow some timing variance

    // Cancel timer
    mgr.cancel(id);
    wait_ms(50);

    int count_after_cancel = fire_count.load();

    // Wait more and verify no additional firings
    wait_ms(100);
    REQUIRE(fire_count.load() == count_after_cancel);

    mgr.stop();
}

TEST_CASE("Timer manager can cancel timer before it fires", "[timer_manager][standalone]") {
    cas::timer_manager mgr;
    mgr.start();

    std::atomic<int> fire_count{0};

    auto msg = std::make_unique<test_tick>();
    auto copier = []() -> std::unique_ptr<cas::message_base> {
        return std::make_unique<test_tick>();
    };

    // Schedule timer for 100ms
    auto id = mgr.schedule(
        std::move(msg),
        std::move(copier),
        std::chrono::milliseconds(100),
        std::chrono::milliseconds(0),
        [&](cas::timer_id, std::unique_ptr<cas::message_base>) {
            fire_count++;
        }
    );

    REQUIRE(mgr.active_count() == 1);

    // Cancel immediately
    mgr.cancel(id);

    REQUIRE(mgr.active_count() == 0);

    // Wait past when it would have fired
    wait_ms(200);

    // Verify it never fired
    REQUIRE(fire_count == 0);

    mgr.stop();
}

TEST_CASE("Timer manager handles multiple concurrent timers", "[timer_manager][standalone]") {
    cas::timer_manager mgr;
    mgr.start();

    std::atomic<int> timer1_fires{0};
    std::atomic<int> timer2_fires{0};
    std::atomic<int> timer3_fires{0};

    // Timer 1: fires at 50ms
    {
        auto msg = std::make_unique<test_tick>();
        auto copier = []() { return std::make_unique<test_tick>(); };
        mgr.schedule(std::move(msg), std::move(copier),
                    std::chrono::milliseconds(50), std::chrono::milliseconds(0),
                    [&](auto, auto) { timer1_fires++; });
    }

    // Timer 2: fires at 100ms
    {
        auto msg = std::make_unique<test_tick>();
        auto copier = []() { return std::make_unique<test_tick>(); };
        mgr.schedule(std::move(msg), std::move(copier),
                    std::chrono::milliseconds(100), std::chrono::milliseconds(0),
                    [&](auto, auto) { timer2_fires++; });
    }

    // Timer 3: fires at 150ms
    {
        auto msg = std::make_unique<test_tick>();
        auto copier = []() { return std::make_unique<test_tick>(); };
        mgr.schedule(std::move(msg), std::move(copier),
                    std::chrono::milliseconds(150), std::chrono::milliseconds(0),
                    [&](auto, auto) { timer3_fires++; });
    }

    REQUIRE(mgr.active_count() == 3);

    // After 75ms, first should have fired
    wait_ms(75);
    REQUIRE(timer1_fires == 1);
    REQUIRE(timer2_fires == 0);
    REQUIRE(timer3_fires == 0);

    // After 125ms total, first two should have fired
    wait_ms(50);
    REQUIRE(timer1_fires == 1);
    REQUIRE(timer2_fires == 1);
    REQUIRE(timer3_fires == 0);

    // After 175ms total, all three should have fired
    wait_ms(50);
    REQUIRE(timer1_fires == 1);
    REQUIRE(timer2_fires == 1);
    REQUIRE(timer3_fires == 1);

    REQUIRE(mgr.active_count() == 0);

    mgr.stop();
}

// Destruction without an explicit stop().
//
// The manager must tear itself down safely when it goes out of scope while the
// worker is still running. This is not just a convenience path: the destructor
// has to join the worker while the mutex, condition variable and containers it
// touches are all still alive. Every other test in this file calls stop()
// first, so without these cases that path is never exercised.
//
// A failure here is a crash, a hang, or a sanitizer report rather than a failed
// assertion - reaching the end of each scope is the result being checked.

TEST_CASE("Timer manager destructs while idle", "[timer_manager][standalone][teardown]") {
    {
        cas::timer_manager mgr;
        mgr.start();
        REQUIRE(mgr.is_running());
        // Destroyed with the worker parked in the empty-queue wait.
    }
    SUCCEED("destroyed while idle without stop()");
}

TEST_CASE("Timer manager destructs with a pending one-shot timer", "[timer_manager][standalone][teardown]") {
    {
        cas::timer_manager mgr;
        mgr.start();

        auto msg = std::make_unique<test_tick>();
        auto copier = []() -> std::unique_ptr<cas::message_base> {
            return std::make_unique<test_tick>();
        };
        // Far enough out that it is still pending at destruction: the worker is
        // in the timed wait, not the empty-queue wait.
        mgr.schedule(std::move(msg), std::move(copier),
                     std::chrono::milliseconds(10000), std::chrono::milliseconds(0),
                     [](auto, auto) {});

        REQUIRE(mgr.active_count() == 1);
    }
    SUCCEED("destroyed with a pending timer without stop()");
}

TEST_CASE("Timer manager destructs with an active periodic timer", "[timer_manager][standalone][teardown]") {
    std::atomic<int> fires{0};
    {
        cas::timer_manager mgr;
        mgr.start();

        auto msg = std::make_unique<test_tick>();
        auto copier = []() -> std::unique_ptr<cas::message_base> {
            return std::make_unique<test_tick>();
        };
        mgr.schedule(std::move(msg), std::move(copier),
                     std::chrono::milliseconds(5), std::chrono::milliseconds(5),
                     [&fires](auto, auto) { fires++; });

        // Let it cycle a few times so destruction lands mid-rescheduling.
        wait_ms(40);
        REQUIRE(fires > 0);
    }
    SUCCEED("destroyed with a periodic timer without stop()");
}

TEST_CASE("Timer manager destructs while a callback is running", "[timer_manager][standalone][teardown]") {
    std::atomic<bool> in_callback{false};
    {
        cas::timer_manager mgr;
        mgr.start();

        auto msg = std::make_unique<test_tick>();
        auto copier = []() -> std::unique_ptr<cas::message_base> {
            return std::make_unique<test_tick>();
        };
        // The callback sleeps, so the destructor runs while the worker is
        // inside user code with the timer mutex released.
        mgr.schedule(std::move(msg), std::move(copier),
                     std::chrono::milliseconds(10), std::chrono::milliseconds(0),
                     [&in_callback](auto, auto) {
                         in_callback = true;
                         std::this_thread::sleep_for(std::chrono::milliseconds(100));
                     });

        // Wait until the callback has actually started before leaving scope.
        for (int i = 0; i < 200 && !in_callback; ++i) {
            wait_ms(1);
        }
        REQUIRE(in_callback);
    }
    SUCCEED("destroyed mid-callback without stop()");
}

TEST_CASE("Timer manager is_running reflects lifecycle", "[timer_manager][standalone][teardown]") {
    cas::timer_manager mgr;
    REQUIRE(!mgr.is_running());

    mgr.start();
    REQUIRE(mgr.is_running());

    // start() on an already-running manager is a no-op, not a restart.
    mgr.start();
    REQUIRE(mgr.is_running());

    mgr.stop();
    REQUIRE(!mgr.is_running());

    // stop() must be idempotent - the destructor calls it again.
    mgr.stop();
    REQUIRE(!mgr.is_running());
}
