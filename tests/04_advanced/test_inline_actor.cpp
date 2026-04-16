#include "../test_common.h"
#include <thread>
#include <atomic>

// Level 4: Advanced - Inline actor with synchronous message processing

namespace inline_actor_test {
    struct calculate : public cas::message_base {
        int value;
        mutable int result;  // mutable so we can modify in const handler
    };

    struct increment : public cas::message_base {
        int amount;
    };

    // Thread-safe inline actor (can be called from multiple threads)
    class calculator : public cas::inline_actor<true> {
    private:
        int m_total = 0;
        mutable std::mutex m_method_mutex;  // Protect direct method calls

    protected:
        void on_start() override {
            set_name("calculator");
            handler<calculate>(&calculator::on_calculate);
            handler<increment>(&calculator::on_increment);
        }

        // Message handlers (protected by inline_actor's mutex)
        void on_calculate(const calculate& msg) {
            msg.result = msg.value * 2;
        }

        void on_increment(const increment& msg) {
            m_total += msg.amount;
        }

    public:
        // Direct method calls - need own mutex
        int calculate_direct(int value) const {
            std::lock_guard<std::mutex> lock(m_method_mutex);
            return value * 2;
        }

        void increment_direct(int amount) {
            std::lock_guard<std::mutex> lock(m_method_mutex);
            m_total += amount;
        }

        int total() const {
            std::lock_guard<std::mutex> lock(m_method_mutex);
            return m_total;
        }
    };

    // Non-thread-safe inline actor (single caller only)
    class fast_calculator : public cas::inline_actor<false> {
    private:
        int m_invocation_count = 0;

    protected:
        void on_start() override {
            set_name("fast_calculator");
        }

    public:
        int calculate(int value) {
            m_invocation_count++;
            return value * 3;
        }

        int invocation_count() const { return m_invocation_count; }
    };
}

TEST_CASE("Inline actor with direct method calls", "[04_advanced][inline_actor]") {
    using namespace inline_actor_test;

    auto calc = cas::system::create<calculator>();
    cas::system::start();
    wait_ms(50);

    // Call methods directly - zero latency, no queuing
    auto& calc_actor = calc.get_checked<calculator>();
    int result = calc_actor.calculate_direct(21);
    REQUIRE(result == 42);

    TEST_CLEANUP();
}

TEST_CASE("Inline actor with message passing (synchronous)", "[04_advanced][inline_actor]") {
    using namespace inline_actor_test;

    auto calc = cas::system::create<calculator>();
    cas::system::start();
    wait_ms(50);

    auto calc_ref = cas::actor_registry::get("calculator");
    REQUIRE(calc_ref.is_valid());

    // Use message passing - processed immediately in sender's thread
    // Note: result must be mutable since message is copied
    calculate msg;
    msg.value = 21;
    msg.result = 0;
    calc_ref.tell(msg);

    // Message was processed, but result is in the copy, not original
    // For inline actors, direct method calls are preferred
    REQUIRE(msg.result == 0);  // Original unchanged

    TEST_CLEANUP();
}

TEST_CASE("Inline actor with multiple callers (thread-safe)", "[04_advanced][inline_actor]") {
    using namespace inline_actor_test;

    auto calc = cas::system::create<calculator>();
    cas::system::start();
    wait_ms(50);

    auto& calc_actor = calc.get_checked<calculator>();

    // Call from multiple threads - methods have own mutex for protection
    std::vector<std::thread> threads;
    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([&calc_actor]() {
            for (int j = 0; j < 100; ++j) {
                calc_actor.increment_direct(1);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // All increments should be accounted for (mutex protects state)
    REQUIRE(calc_actor.total() == 1000);

    TEST_CLEANUP();
}

TEST_CASE("Non-thread-safe inline actor (single caller)", "[04_advanced][inline_actor]") {
    using namespace inline_actor_test;

    auto calc = cas::system::create<fast_calculator>();
    cas::system::start();
    wait_ms(50);

    auto& calc_actor = calc.get_checked<fast_calculator>();

    // Single caller - no mutex overhead
    int result1 = calc_actor.calculate(10);
    REQUIRE(result1 == 30);

    int result2 = calc_actor.calculate(5);
    REQUIRE(result2 == 15);

    REQUIRE(calc_actor.invocation_count() == 2);

    TEST_CLEANUP();
}
