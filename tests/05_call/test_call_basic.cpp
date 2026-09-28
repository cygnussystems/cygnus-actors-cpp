#include "../test_common.h"

// Level 5: Test call pattern (RPC-style synchronous request-response)

// Operation tags for call pattern
struct add_op {};
struct multiply_op {};
struct divide_op {};

// Simple trigger message for actor-to-actor test
struct trigger_msg : public cas::message_base {};

// Calculator actor that responds to calls
class calculator_actor : public cas::actor {
protected:
    void on_start() override {
        set_name("calculator");

        // Register call handlers for different operations
        call_handler<int, add_op>(&calculator_actor::add);
        call_handler<int, multiply_op>(&calculator_actor::multiply);
        call_handler<double, divide_op>(&calculator_actor::divide);
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

TEST_CASE("Simple call with integer return", "[05_call][basic]") {
    CAS_TEST_GUARD();
    auto calc = cas::system::create<calculator_actor>();

    cas::system::start();
    wait_ms(50);

    // Call for addition
    int result = calc.call<int>(add_op{}, 10, 5);

    REQUIRE(result == 15);

    TEST_CLEANUP();
}

TEST_CASE("Multiple call operations", "[05_call][basic]") {
    CAS_TEST_GUARD();
    auto calc = cas::system::create<calculator_actor>();

    cas::system::start();
    wait_ms(50);

    // Multiple operations
    int sum = calc.call<int>(add_op{}, 100, 50);
    int product = calc.call<int>(multiply_op{}, 10, 5);
    double quotient = calc.call<double>(divide_op{}, 20, 4);

    REQUIRE(sum == 150);
    REQUIRE(product == 50);
    REQUIRE(quotient == 5.0);

    TEST_CLEANUP();
}

// An actor may call() another actor that runs on a different worker thread:
// the caller's worker blocks while the target's worker handles the call. The
// two actors here are created back to back, so round-robin placement puts them
// on different workers. (A call to an actor on the caller's own worker would
// deadlock, and throws call_deadlock_error instead.)
TEST_CASE("Call from one actor to another", "[05_call][basic]") {
    CAS_TEST_GUARD();
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
            // Call calculator from within actor
            result = calc_ref.call<int>(add_op{}, 7, 3);
        }
    };

    auto calc = cas::system::create<calculator_actor>();
    auto requester = cas::system::create<requester_actor>(calc);

    cas::system::start();
    wait_ms(50);

    // Trigger the call
    requester << trigger_msg{};
    wait_ms(100);

    auto* req_ptr = requester.get<requester_actor>();
    REQUIRE(req_ptr != nullptr);
    REQUIRE(req_ptr->get_result() == 10);

    TEST_CLEANUP();
}

TEST_CASE("Call blocks until result ready", "[05_call][blocking]") {
    CAS_TEST_GUARD();
    class slow_actor : public cas::actor {
    protected:
        void on_start() override {
            set_name("slow");
            call_handler<int, add_op>(&slow_actor::slow_add);
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
    int result = slow.call<int>(add_op{}, 5, 3);
    auto elapsed = std::chrono::steady_clock::now() - start;

    REQUIRE(result == 8);
    // Should have taken at least 100ms
    REQUIRE(elapsed >= std::chrono::milliseconds(90));

    TEST_CLEANUP();
}

TEST_CASE("Call with no handler throws exception", "[05_call][error]") {
    CAS_TEST_GUARD();
    struct unknown_op {};

    auto calc = cas::system::create<calculator_actor>();

    cas::system::start();
    wait_ms(50);

    // Try to call unregistered operation
    REQUIRE_THROWS_AS(
        calc.call<int>(unknown_op{}, 5, 3),
        std::runtime_error
    );

    TEST_CLEANUP();
}

TEST_CASE("Call handler exception propagates to caller", "[05_call][error]") {
    CAS_TEST_GUARD();
    auto calc = cas::system::create<calculator_actor>();

    cas::system::start();
    wait_ms(50);

    // Division by zero should throw
    REQUIRE_THROWS_AS(
        calc.call<double>(divide_op{}, 10, 0),
        std::runtime_error
    );

    TEST_CLEANUP();
}
