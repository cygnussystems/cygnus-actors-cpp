#include "../test_common.h"
#include <atomic>

// Level 11: queue threshold warnings are actually delivered.
//
// Previously enqueue_message() logged the breach only under CAS_DEBUG_LOGGING,
// so in a release build breaching the threshold did nothing at all. Silent
// degradation under load is exactly what an operational monitoring feature must
// not do. The callback now mirrors set_dead_letter_handler().

namespace threshold_test {
    struct slow_msg : public cas::message_base {};

    // Blocks on first message so the mailbox backs up deterministically
    class backlog_actor : public cas::actor {
    public:
        std::atomic<bool> release{false};
    protected:
        void on_start() override {
            set_name("backlog_actor");
            handler<slow_msg>([this](const slow_msg&) {
                while (!release.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            });
        }
    };
}

TEST_CASE("Queue threshold breach invokes the handler", "[11_concurrency][threshold]") {
    using namespace threshold_test;

    std::atomic<int> callback_count{0};
    std::atomic<size_t> reported_threshold{0};
    std::string reported_name;
    std::mutex name_mutex;

    // A previous test may have left the system running; configure() rejects
    // that. Ensure a clean slate before touching configuration.
    TEST_CLEANUP();

    cas::system_config config;
    config.queue_threshold = 20;
    cas::system::configure(config);

    cas::system::set_queue_threshold_handler(
        [&](const cas::queue_threshold_info& info) {
            callback_count.fetch_add(1);
            reported_threshold.store(info.threshold);
            std::lock_guard<std::mutex> lock(name_mutex);
            reported_name = info.actor_name;
        });

    auto actor = cas::system::create<backlog_actor>();
    cas::system::start();
    wait_ms(50);

    auto ref = cas::actor_registry::get("backlog_actor");
    REQUIRE(ref.is_valid());

    // First message parks the handler, the rest pile up past the threshold
    for (int i = 0; i < 100; ++i) {
        ref.tell(slow_msg{});
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (callback_count.load() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        wait_ms(10);
    }

    // Let the actor drain before shutdown
    actor.get_checked<backlog_actor>().release.store(true);

    REQUIRE(callback_count.load() >= 1);
    REQUIRE(reported_threshold.load() == 20);
    {
        std::lock_guard<std::mutex> lock(name_mutex);
        REQUIRE(reported_name == "backlog_actor");
    }

    // Fires once per breach, not once per message
    REQUIRE(callback_count.load() == 1);

    cas::system::clear_queue_threshold_handler();
    TEST_CLEANUP();

    // Restore default configuration so later tests are unaffected
    cas::system::configure(cas::system_config{});
}

TEST_CASE("No threshold handler is safe", "[11_concurrency][threshold]") {
    using namespace threshold_test;

    TEST_CLEANUP();

    cas::system_config config;
    config.queue_threshold = 5;
    cas::system::configure(config);
    cas::system::clear_queue_threshold_handler();

    auto actor = cas::system::create<backlog_actor>();
    cas::system::start();
    wait_ms(50);

    auto ref = cas::actor_registry::get("backlog_actor");
    REQUIRE(ref.is_valid());

    for (int i = 0; i < 50; ++i) {
        ref.tell(slow_msg{});
    }
    wait_ms(100);

    actor.get_checked<backlog_actor>().release.store(true);
    wait_ms(100);

    SUCCEED("Breaching the threshold with no handler installed did not crash");

    TEST_CLEANUP();
    cas::system::configure(cas::system_config{});
}
