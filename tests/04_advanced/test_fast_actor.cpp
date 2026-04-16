#include "../test_common.h"
#include <chrono>

// Level 4: Advanced - Fast actor with dedicated thread and low latency

namespace fast_actor_test {
    struct tick : public cas::message_base {
        std::chrono::steady_clock::time_point sent_time;
    };

    struct stop : public cas::message_base {};

    class low_latency_actor : public cas::fast_actor {
    private:
        int m_tick_count = 0;
        std::chrono::microseconds m_total_latency{0};
        std::chrono::microseconds m_max_latency{0};

    protected:
        void on_start() override {
            set_name("low_latency");
            handler<tick>(&low_latency_actor::on_tick);
            handler<stop>(&low_latency_actor::on_stop_msg);
            // Default: yield strategy
        }

        void on_tick(const tick& msg) {
            auto now = std::chrono::steady_clock::now();
            auto latency = std::chrono::duration_cast<std::chrono::microseconds>(now - msg.sent_time);

            m_tick_count++;
            m_total_latency += latency;
            if (latency > m_max_latency) {
                m_max_latency = latency;
            }
        }

        void on_stop_msg(const stop& /*msg*/) {
            // Will be handled by test
        }

    public:
        low_latency_actor() = default;
        explicit low_latency_actor(cas::polling_strategy strategy) : cas::fast_actor(strategy) {}

        int tick_count() const { return m_tick_count; }

        std::chrono::microseconds avg_latency() const {
            if (m_tick_count == 0) return std::chrono::microseconds(0);
            return m_total_latency / m_tick_count;
        }

        std::chrono::microseconds max_latency() const { return m_max_latency; }
    };
}

TEST_CASE("Fast actor processes messages with low latency", "[04_advanced][fast_actor]") {
    using namespace fast_actor_test;

    auto actor = cas::system::create<low_latency_actor>();
    cas::system::start();
    wait_ms(50);  // Let actor start

    auto actor_ref = cas::actor_registry::get("low_latency");
    REQUIRE(actor_ref.is_valid());

    // Send multiple tick messages and measure latency
    const int num_ticks = 100;
    for (int i = 0; i < num_ticks; ++i) {
        tick t;
        t.sent_time = std::chrono::steady_clock::now();
        actor_ref.tell(t);
    }

    // Wait for processing
    wait_ms(100);

    auto& la = actor.get_checked<low_latency_actor>();
    REQUIRE(la.tick_count() == num_ticks);

    TEST_CLEANUP();
}

TEST_CASE("Fast actor with yield strategy (default)", "[04_advanced][fast_actor]") {
    using namespace fast_actor_test;

    // Yield strategy - cooperative CPU usage
    auto actor = cas::system::create<low_latency_actor>(cas::polling_strategy::yield);
    cas::system::start();
    wait_ms(50);

    auto actor_ref = cas::actor_registry::get("low_latency");
    REQUIRE(actor_ref.is_valid());

    // Send some messages
    for (int i = 0; i < 10; ++i) {
        tick t;
        t.sent_time = std::chrono::steady_clock::now();
        actor_ref.tell(t);
    }

    wait_ms(50);
    auto& la = actor.get_checked<low_latency_actor>();
    REQUIRE(la.tick_count() == 10);

    TEST_CLEANUP();
}

TEST_CASE("Fast actor with hybrid strategy", "[04_advanced][fast_actor]") {
    using namespace fast_actor_test;

    // Hybrid strategy - spin then yield
    auto actor = cas::system::create<low_latency_actor>(cas::polling_strategy::hybrid);
    cas::system::start();
    wait_ms(50);

    auto actor_ref = cas::actor_registry::get("low_latency");
    REQUIRE(actor_ref.is_valid());

    // Send messages rapidly
    for (int i = 0; i < 20; ++i) {
        tick t;
        t.sent_time = std::chrono::steady_clock::now();
        actor_ref.tell(t);
    }

    wait_ms(50);
    auto& la = actor.get_checked<low_latency_actor>();
    REQUIRE(la.tick_count() == 20);

    TEST_CLEANUP();
}

TEST_CASE("Fast actor handles shutdown correctly", "[04_advanced][fast_actor]") {
    using namespace fast_actor_test;

    auto actor = cas::system::create<low_latency_actor>();
    cas::system::start();
    wait_ms(50);

    auto actor_ref = cas::actor_registry::get("low_latency");
    REQUIRE(actor_ref.is_valid());

    // Send some messages
    for (int i = 0; i < 10; ++i) {
        tick t;
        t.sent_time = std::chrono::steady_clock::now();
        actor_ref.tell(t);
    }

    wait_ms(50);

    auto& la = actor.get_checked<low_latency_actor>();
    REQUIRE(la.tick_count() == 10);

    TEST_CLEANUP();
}
