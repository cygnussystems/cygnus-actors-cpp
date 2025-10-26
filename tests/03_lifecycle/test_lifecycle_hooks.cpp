#include "../test_common.h"

// Level 3: Actor lifecycle hook testing

namespace lifecycle_test {
    class lifecycle_tracker : public cas::actor {
    private:
        std::atomic<bool>* on_start_called_;
        std::atomic<bool>* on_shutdown_called_;
        std::atomic<bool>* on_stop_called_;
        std::atomic<int>* call_order_;

    public:
        lifecycle_tracker(std::atomic<bool>* start, std::atomic<bool>* shutdown,
                         std::atomic<bool>* stop, std::atomic<int>* order)
            : on_start_called_(start), on_shutdown_called_(shutdown),
              on_stop_called_(stop), call_order_(order) {}

    protected:
        void on_start() override {
            on_start_called_->store(true);
            int expected = 0;
            call_order_->compare_exchange_strong(expected, 1);
        }

        void on_shutdown() override {
            on_shutdown_called_->store(true);
            int expected = 1;
            call_order_->compare_exchange_strong(expected, 2);
        }

        void on_stop() override {
            on_stop_called_->store(true);
            int expected = 2;
            call_order_->compare_exchange_strong(expected, 3);
        }
    };
}

TEST_CASE("on_start is called when system starts", "[03_lifecycle][hooks]") {
    std::atomic<bool> on_start_called{false};
    std::atomic<bool> on_shutdown_called{false};
    std::atomic<bool> on_stop_called{false};
    std::atomic<int> call_order{0};

    auto actor = cas::system::create<lifecycle_test::lifecycle_tracker>(
        &on_start_called, &on_shutdown_called, &on_stop_called, &call_order
    );

    REQUIRE(!on_start_called.load());

    cas::system::start();
    wait_ms(50);

    REQUIRE(on_start_called.load());
    REQUIRE(!on_shutdown_called.load());
    REQUIRE(!on_stop_called.load());

    TEST_CLEANUP();
}

TEST_CASE("All lifecycle hooks are called in order", "[03_lifecycle][hooks]") {
    std::atomic<bool> on_start_called{false};
    std::atomic<bool> on_shutdown_called{false};
    std::atomic<bool> on_stop_called{false};
    std::atomic<int> call_order{0};

    auto actor = cas::system::create<lifecycle_test::lifecycle_tracker>(
        &on_start_called, &on_shutdown_called, &on_stop_called, &call_order
    );

    cas::system::start();
    wait_ms(50);

    cas::system::shutdown();
    cas::system::wait_for_shutdown();

    // Verify all called
    REQUIRE(on_start_called.load());
    REQUIRE(on_shutdown_called.load());
    REQUIRE(on_stop_called.load());

    // Verify order: on_start -> on_shutdown -> on_stop
    REQUIRE(call_order.load() == 3);

    TEST_CLEANUP();
}

TEST_CASE("Multiple actors all complete lifecycle", "[03_lifecycle][hooks]") {
    const int NUM_ACTORS = 3;
    std::vector<std::atomic<bool>> starts(NUM_ACTORS);
    std::vector<std::atomic<bool>> shutdowns(NUM_ACTORS);
    std::vector<std::atomic<bool>> stops(NUM_ACTORS);
    std::vector<std::atomic<int>> orders(NUM_ACTORS);

    for (int i = 0; i < NUM_ACTORS; ++i) {
        starts[i].store(false);
        shutdowns[i].store(false);
        stops[i].store(false);
        orders[i].store(0);
    }

    // Create multiple actors
    for (int i = 0; i < NUM_ACTORS; ++i) {
        cas::system::create<lifecycle_test::lifecycle_tracker>(
            &starts[i], &shutdowns[i], &stops[i], &orders[i]
        );
    }

    cas::system::start();
    wait_ms(100);

    cas::system::shutdown();
    cas::system::wait_for_shutdown();

    // Verify all actors completed lifecycle
    for (int i = 0; i < NUM_ACTORS; ++i) {
        REQUIRE(starts[i].load());
        REQUIRE(shutdowns[i].load());
        REQUIRE(stops[i].load());
        REQUIRE(orders[i].load() == 3);
    }

    TEST_CLEANUP();
}
