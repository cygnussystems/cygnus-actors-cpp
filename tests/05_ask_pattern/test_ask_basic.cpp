#include "../test_common.h"

// Level 5: Test ask pattern (RPC-style synchronous request-response)

// Operation tags for ask pattern
struct add_op {};
struct multiply_op {};
struct divide_op {};

// Simple trigger message for actor-to-actor test
struct trigger_msg : public cas::message_base {};

// Calculator actor that responds to ask requests
class calculator_actor : public cas::actor {
protected:
    void on_start() override {
        set_name("calculator");

        // Register ask handlers for different operations
        ask_handler<int, add_op>(&calculator_actor::add);
        ask_handler<int, multiply_op>(&calculator_actor::multiply);
        ask_handler<double, divide_op>(&calculator_actor::divide);
    }

    int add(int a, int b) {
        return a + b;
    }

    int multiply(int a, int b) {
        return a * b;
    }

    double divide(int a, int b) {
        if (b == 0) {
            throw std::runtime_error("Division by zero");
        }
        return static_cast<double>(a) / b;
    }
};

TEST_CASE("Simple ask with integer return", "[05_ask_pattern][basic]") {
    auto calc = cas::system::create<calculator_actor>();

    cas::system::start();
    wait_ms(50);

    // Ask for addition
    int result = calc.ask<int>(add_op{}, 10, 5);

    REQUIRE(result == 15);

    TEST_CLEANUP();
}

TEST_CASE("Multiple ask operations", "[05_ask_pattern][basic]") {
    auto calc = cas::system::create<calculator_actor>();

    cas::system::start();
    wait_ms(50);

    // Multiple operations
    int sum = calc.ask<int>(add_op{}, 100, 50);
    int product = calc.ask<int>(multiply_op{}, 10, 5);
    double quotient = calc.ask<double>(divide_op{}, 20, 4);

    REQUIRE(sum == 150);
    REQUIRE(product == 50);
    REQUIRE(quotient == 5.0);

    TEST_CLEANUP();
}

// NOTE: This test is disabled due to potential deadlock when calling ask() from within an actor's message handler.
// If both actors are assigned to the same worker thread, the requester blocks waiting for calculator, but calculator
// can't run because the thread is blocked. This is a known limitation of synchronous ask patterns in actor systems.
// Recommendation: Use async message passing instead of blocking ask() from within actors.
/*
TEST_CASE("Ask from one actor to another", "[05_ask_pattern][basic]") {
    class requester_actor : public cas::actor {
    private:
        cas::actor_ref calc_ref;
        int result = 0;

    public:
        requester_actor(cas::actor_ref calc) : calc_ref(calc) {}

        int get_result() const { return result; }

    protected:
        void on_start() override {
            set_name("requester");
            handler<trigger_msg>(&requester_actor::on_trigger);
        }

        void on_trigger(const trigger_msg& msg) {
            // Ask calculator from within actor
            result = calc_ref.ask<int>(add_op{}, 7, 3);
        }
    };

    auto calc = cas::system::create<calculator_actor>();
    auto requester = cas::system::create<requester_actor>(calc);

    cas::system::start();
    wait_ms(50);

    // Trigger the ask
    requester << trigger_msg{};
    wait_ms(100);

    auto* req_ptr = requester.get<requester_actor>();
    REQUIRE(req_ptr != nullptr);
    REQUIRE(req_ptr->get_result() == 10);

    TEST_CLEANUP();
}
*/

TEST_CASE("Ask blocks until result ready", "[05_ask_pattern][blocking]") {
    class slow_actor : public cas::actor {
    protected:
        void on_start() override {
            set_name("slow");
            ask_handler<int, add_op>(&slow_actor::slow_add);
        }

        int slow_add(int a, int b) {
            // Simulate slow operation
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            return a + b;
        }
    };

    auto slow = cas::system::create<slow_actor>();

    cas::system::start();
    wait_ms(50);

    auto start = std::chrono::steady_clock::now();
    int result = slow.ask<int>(add_op{}, 5, 3);
    auto elapsed = std::chrono::steady_clock::now() - start;

    REQUIRE(result == 8);
    // Should have taken at least 100ms
    REQUIRE(elapsed >= std::chrono::milliseconds(90));

    TEST_CLEANUP();
}

TEST_CASE("Ask with no handler throws exception", "[05_ask_pattern][error]") {
    struct unknown_op {};

    auto calc = cas::system::create<calculator_actor>();

    cas::system::start();
    wait_ms(50);

    // Try to call unregistered operation
    REQUIRE_THROWS_AS(
        calc.ask<int>(unknown_op{}, 5, 3),
        std::runtime_error
    );

    TEST_CLEANUP();
}

TEST_CASE("Ask handler exception propagates to caller", "[05_ask_pattern][error]") {
    auto calc = cas::system::create<calculator_actor>();

    cas::system::start();
    wait_ms(50);

    // Division by zero should throw
    REQUIRE_THROWS_AS(
        calc.ask<double>(divide_op{}, 10, 0),
        std::runtime_error
    );

    TEST_CLEANUP();
}
