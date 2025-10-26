#include "../test_common.h"

// Level 5: Test ask pattern timeout functionality

struct slow_op {};

class slow_responder : public cas::actor {
protected:
    void on_start() override {
        set_name("slow_responder");
        ask_handler<int, slow_op>(&slow_responder::slow_operation);
    }

    int slow_operation(int delay_ms, int value) {
        // Simulate slow operation
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        return value * 2;
    }
};

TEST_CASE("Ask with timeout returns value when fast enough", "[05_ask_pattern][timeout]") {
    auto slow = cas::system::create<slow_responder>();

    cas::system::start();
    wait_ms(50);

    // Fast operation with generous timeout
    auto result = slow.ask<int>(slow_op{}, std::chrono::milliseconds(1000), 50, 42);

    REQUIRE(result.has_value());
    REQUIRE(result.value() == 84);

    TEST_CLEANUP();
}

TEST_CASE("Ask with timeout returns nullopt when too slow", "[05_ask_pattern][timeout]") {
    auto slow = cas::system::create<slow_responder>();

    cas::system::start();
    wait_ms(50);

    // Slow operation (500ms) with short timeout (100ms)
    auto result = slow.ask<int>(slow_op{}, std::chrono::milliseconds(100), 500, 42);

    REQUIRE(!result.has_value());

    TEST_CLEANUP();
}

TEST_CASE("Ask timeout does not block forever", "[05_ask_pattern][timeout]") {
    auto slow = cas::system::create<slow_responder>();

    cas::system::start();
    wait_ms(50);

    auto start = std::chrono::steady_clock::now();

    // Very slow operation (1000ms) with short timeout (200ms)
    auto result = slow.ask<int>(slow_op{}, std::chrono::milliseconds(200), 1000, 42);

    auto elapsed = std::chrono::steady_clock::now() - start;

    REQUIRE(!result.has_value());
    // Should have timed out around 200ms, not waited full 1000ms
    REQUIRE(elapsed < std::chrono::milliseconds(500));
    REQUIRE(elapsed >= std::chrono::milliseconds(190));

    TEST_CLEANUP();
}

TEST_CASE("Multiple asks with different timeouts", "[05_ask_pattern][timeout]") {
    auto slow = cas::system::create<slow_responder>();

    cas::system::start();
    wait_ms(50);

    // Fast call - should succeed
    auto result1 = slow.ask<int>(slow_op{}, std::chrono::milliseconds(500), 50, 10);
    REQUIRE(result1.has_value());
    REQUIRE(result1.value() == 20);

    // Slow call - should timeout
    auto result2 = slow.ask<int>(slow_op{}, std::chrono::milliseconds(100), 500, 20);
    REQUIRE(!result2.has_value());

    // Another fast call - should succeed even after timeout
    auto result3 = slow.ask<int>(slow_op{}, std::chrono::milliseconds(500), 50, 30);
    REQUIRE(result3.has_value());
    REQUIRE(result3.value() == 60);

    TEST_CLEANUP();
}
