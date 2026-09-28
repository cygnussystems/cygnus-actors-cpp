#include "../test_common.h"
#include <atomic>
#include <string>
#include <thread>
#include <vector>

// Issue 008: every exposed metric has a writer (INVARIANT 4).
// Issue 009: name() is safe to read from any thread; the registry does not
// own actors; timer messages carry a message id; finished one-shot timers are
// not tracked forever.

namespace metrics_test {
    struct slow_msg : public cas::message_base {};
    struct noop : public cas::message_base {};
    struct get_op {};

    class gated_actor : public cas::actor {
    public:
        std::atomic<bool> release{false};
        std::atomic<int> handled{0};
    protected:
        void on_start() override {
            set_name("gated_actor");
            handler<slow_msg>([this](const slow_msg&) {
                while (!release.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                handled++;
            });
        }
    };

    class named_actor : public cas::actor {
    protected:
        void on_start() override {
            handler<noop>([](const noop&) {});
        }
    public:
        void rename(const std::string& n) { set_name(n); }
    };

    struct tick : public cas::message_base {};

    class tick_recorder : public cas::actor {
    public:
        std::atomic<uint64_t> last_id{0};
        std::atomic<int> ticks{0};
    protected:
        void on_start() override {
            handler<tick>([this](const tick& t) {
                last_id = t.message_id;
                ticks++;
            });
            schedule_once(std::chrono::milliseconds(1), tick{});
        }
    };

    template<typename Pred>
    bool wait_for(Pred pred) {
        for (int i = 0; i < 400 && !pred(); ++i) {
            wait_ms(5);
        }
        return pred();
    }
}

using namespace metrics_test;

TEST_CASE("tell() and call(timeout) on an invalid actor_ref count as dead letters",
          "[11_concurrency][metrics][issue008]") {
    CAS_CONFIG_GUARD();
    cas::system::reset_dead_letter_stats();

    std::atomic<int> handler_calls{0};
    cas::system::set_dead_letter_handler([&](const cas::dead_letter_info& info) {
        if (info.target_actor_name.empty()) {
            handler_calls++;
        }
    });

    cas::actor_ref invalid;
    invalid.tell(noop{});
    invalid.tell(noop{});
    REQUIRE_FALSE(invalid.call<int>(get_op{}, std::chrono::milliseconds(10)).has_value());

    auto stats = cas::system::get_dead_letter_stats();
    REQUIRE(stats.dropped_to_invalid == 3);
    REQUIRE(stats.dropped_tell == 0);
    REQUIRE(handler_calls.load() == 3);
}

TEST_CASE("Queue threshold is reported again after the queue recovers",
          "[11_concurrency][threshold][issue008]") {
    CAS_CONFIG_GUARD();
    cas::system_config config;
    config.queue_threshold = 20;
    cas::system::configure(config);

    std::atomic<int> breaches{0};
    cas::system::set_queue_threshold_handler([&](const cas::queue_threshold_info&) { breaches++; });

    auto ref = cas::system::create<gated_actor>();
    cas::system::start();
    auto& a = ref.get_checked<gated_actor>();

    for (int round = 1; round <= 2; ++round) {
        // In round 2 the first of these finds the queue empty, which re-arms
        // the warning; the 21st queued message breaches again.
        a.release = false;
        for (int i = 0; i < 50; ++i) {
            ref.tell(slow_msg{});
        }
        REQUIRE(wait_for([&] { return breaches.load() == round; }));
        a.release = true;
        REQUIRE(wait_for([&] { return a.handled.load() == 50 * round; }));
    }
    REQUIRE(breaches.load() == 2);  // once per breach, not per message
}

TEST_CASE("total_high_water_mark is the largest total seen, not a sum of maxima",
          "[11_concurrency][metrics][issue008]") {
    CAS_TEST_GUARD();
    auto ref = cas::system::create<gated_actor>();
    cas::system::start();
    auto& a = ref.get_checked<gated_actor>();

    for (int i = 0; i < 30; ++i) {
        ref.tell(slow_msg{});
    }
    a.release = true;
    REQUIRE(wait_for([&] { return a.handled.load() == 30; }));

    REQUIRE(a.mailbox_high_water_mark() >= 29);
    REQUIRE(a.total_high_water_mark() >= a.mailbox_high_water_mark());
    REQUIRE(a.total_high_water_mark() <= 30);
}

TEST_CASE("An actor has its default name as soon as it is created",
          "[11_concurrency][naming][issue009]") {
    CAS_TEST_GUARD();
    auto ref = cas::system::create<named_actor>();
    auto& a = ref.get_checked<named_actor>();

    const std::string& n = a.name();
    REQUIRE(n.find("named_actor") != std::string::npos);
    REQUIRE(n == a.type_name() + "_" + std::to_string(a.instance_id()));
}

TEST_CASE("name() can be read while the actor renames itself",
          "[11_concurrency][naming][issue009]") {
    CAS_TEST_GUARD();
    auto ref = cas::system::create<named_actor>();
    auto& a = ref.get_checked<named_actor>();
    cas::system::start();

    std::atomic<bool> stop{false};
    std::atomic<size_t> reads{0};
    std::thread reader([&] {
        while (!stop.load()) {
            const std::string& n = a.name();  // stays valid across renames
            reads += n.size() > 0 ? 1 : 0;
        }
    });
    // Make the reads overlap the renames: wait for the reader to be running
    while (reads.load() == 0) {
        std::this_thread::yield();
    }
    for (int i = 0; i < 200; ++i) {
        a.rename("renamed_" + std::to_string(i));
    }
    stop = true;
    reader.join();

    REQUIRE(a.name() == "renamed_199");
    REQUIRE(reads.load() > 0);
    // A rename releases the old name
    REQUIRE_FALSE(cas::actor_registry::exists("renamed_198"));
    REQUIRE(cas::actor_registry::get("renamed_199").is_valid());
}

TEST_CASE("The registry does not keep an actor alive",
          "[11_concurrency][registry][issue009]") {
    CAS_TEST_GUARD();
    auto owner = std::make_shared<named_actor>();
    cas::actor_registry::register_actor("weakly_held", owner);
    REQUIRE(cas::actor_registry::exists("weakly_held"));

    std::weak_ptr<named_actor> watch = owner;
    owner.reset();
    REQUIRE(watch.expired());
    REQUIRE_FALSE(cas::actor_registry::exists("weakly_held"));
    REQUIRE_FALSE(cas::actor_registry::get("weakly_held").is_valid());
}

TEST_CASE("Timer messages carry a message id",
          "[11_concurrency][timers][issue009]") {
    CAS_TEST_GUARD();
    auto ref = cas::system::create<tick_recorder>();
    cas::system::start();
    auto& a = ref.get_checked<tick_recorder>();
    REQUIRE(wait_for([&] { return a.ticks.load() == 1; }));
    REQUIRE(a.last_id.load() != 0);
}
