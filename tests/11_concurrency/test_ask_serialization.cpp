#include "../test_common.h"
#include <vector>
#include <thread>

// Level 11: Concurrency invariants.
//
// These tests exist to catch specific defects that the original suite could not
// express, because every test there drove a single actor from the main thread
// and asserted on a counter after a fixed sleep.
//
// Each test here names the invariant it defends. See doc/INVARIANTS.md.

namespace ask_serialization_test {

    struct increment_msg : public cas::message_base {};
    struct bump_op {};

    // INVARIANT 1 probe.
    //
    // m_counter is deliberately a PLAIN int, not an atomic. If an ask handler
    // can run concurrently with a regular handler on the same actor, the
    // read-modify-write races and the final total comes out below 2N.
    //
    // Under the pre-fix implementation (ask requests dispatched directly from a
    // dedicated ask thread while a worker thread ran the mailbox) this test
    // fails under load. It must never fail again.
    class counter_actor : public cas::actor {
    private:
        int m_counter = 0;  // intentionally NOT atomic - that is the point

    public:
        int counter() const { return m_counter; }

    protected:
        void on_start() override {
            set_name("counter_actor");
            handler<increment_msg>(&counter_actor::on_increment);
            ask_handler<int, bump_op>(&counter_actor::on_bump);
        }

        void on_increment(const increment_msg&) {
            ++m_counter;
        }

        int on_bump() {
            ++m_counter;
            return m_counter;
        }
    };
}

TEST_CASE("Ask handlers are serialised with regular handlers", "[11_concurrency][invariant1]") {
    CAS_TEST_GUARD();
    using namespace ask_serialization_test;

    constexpr int kPerThread = 400;
    constexpr int kTellThreads = 4;
    constexpr int kAskThreads = 4;

    auto counter = cas::system::create<counter_actor>();
    cas::system::start();
    wait_ms(50);

    auto ref = cas::actor_registry::get("counter_actor");
    REQUIRE(ref.is_valid());

    std::vector<std::thread> threads;
    std::atomic<int> ask_failures{0};

    // Fire-and-forget senders
    for (int t = 0; t < kTellThreads; ++t) {
        threads.emplace_back([ref, n = kPerThread]() {
            for (int i = 0; i < n; ++i) {
                ref.tell(increment_msg{});
            }
        });
    }

    // Ask senders - these block on the actor's own thread servicing the request
    for (int t = 0; t < kAskThreads; ++t) {
        threads.emplace_back([ref, &ask_failures, n = kPerThread]() mutable {
            for (int i = 0; i < n; ++i) {
                try {
                    ref.ask<int>(bump_op{});
                } catch (const std::exception&) {
                    ask_failures.fetch_add(1);
                }
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    REQUIRE(ask_failures.load() == 0);

    // Drain the mailbox: asks are synchronous but tells are not
    constexpr int kExpected = (kTellThreads + kAskThreads) * kPerThread;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    auto& actor_obj = counter.get_checked<counter_actor>();
    while (actor_obj.counter() < kExpected &&
           std::chrono::steady_clock::now() < deadline) {
        wait_ms(10);
    }

    // Exactly 2N - no lost updates. A racing implementation lands below this.
    REQUIRE(actor_obj.counter() == kExpected);

    TEST_CLEANUP();
}

TEST_CASE("Only one thread is inside an actor's handlers at a time",
          "[11_concurrency][invariant1]") {
    CAS_TEST_GUARD();
    using namespace ask_serialization_test;

    // Complements the 2N counter test above. That one detects a race only
    // AFTER it has corrupted data; this one observes the mechanism directly by
    // counting concurrent entries into the actor's handlers. It fails on the
    // first overlap, whether or not any increment was actually lost.
    //
    // Note this duplicates some of the driving code above deliberately: the two
    // tests assert different properties and must both stand on their own.
    struct observed_actor : public cas::actor {
        std::atomic<int> concurrent_entries{0};   // live count inside a handler
        std::atomic<int> max_concurrent{0};       // high-water mark observed
        std::atomic<int> total_dispatches{0};

        void enter() {
            int now = concurrent_entries.fetch_add(1) + 1;
            int prev = max_concurrent.load();
            while (now > prev &&
                   !max_concurrent.compare_exchange_weak(prev, now)) {
                // retry until the high-water mark reflects this entry
            }
            // Hold the window open briefly so a genuine overlap is observable
            std::this_thread::sleep_for(std::chrono::microseconds(20));
        }

        void leave() {
            concurrent_entries.fetch_sub(1);
            total_dispatches.fetch_add(1);
        }

        void on_start() override {
            set_name("observed_actor");
            handler<increment_msg>([this](const increment_msg&) {
                enter();
                leave();
            });
            ask_handler<int, bump_op>(&observed_actor::on_bump);
        }

        int on_bump() {
            enter();
            leave();
            return 0;
        }
    };

    constexpr int kPerThread = 150;
    constexpr int kTellThreads = 3;
    constexpr int kAskThreads = 3;

    auto a = cas::system::create<observed_actor>();
    cas::system::start();
    wait_ms(50);

    auto ref = cas::actor_registry::get("observed_actor");
    REQUIRE(ref.is_valid());

    std::vector<std::thread> threads;
    for (int t = 0; t < kTellThreads; ++t) {
        threads.emplace_back([ref, n = kPerThread]() {
            for (int i = 0; i < n; ++i) ref.tell(increment_msg{});
        });
    }
    for (int t = 0; t < kAskThreads; ++t) {
        threads.emplace_back([ref, n = kPerThread]() mutable {
            for (int i = 0; i < n; ++i) {
                try { ref.ask<int>(bump_op{}); } catch (const std::exception&) {}
            }
        });
    }
    for (auto& th : threads) th.join();

    constexpr int kExpected = (kTellThreads + kAskThreads) * kPerThread;
    auto& obj = a.get_checked<observed_actor>();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (obj.total_dispatches.load() < kExpected &&
           std::chrono::steady_clock::now() < deadline) {
        wait_ms(10);
    }

    REQUIRE(obj.total_dispatches.load() == kExpected);

    // The invariant: never more than one thread inside this actor's handlers.
    REQUIRE(obj.max_concurrent.load() == 1);

    TEST_CLEANUP();
}

TEST_CASE("Ask that would deadlock throws instead of hanging", "[11_concurrency][invariant1]") {
    CAS_TEST_GUARD();
    using namespace ask_serialization_test;

    // An actor asking a target pinned to its own worker thread would block that
    // thread forever, since the same thread must service the request. That is a
    // programming error and must surface as an exception, not a hang.
    struct self_ask_actor : public cas::actor {
        std::atomic<bool> threw{false};
        std::atomic<bool> completed{false};

        void on_start() override {
            set_name("self_ask_actor");
            ask_handler<int, bump_op>(&self_ask_actor::on_bump);
            handler<increment_msg>(&self_ask_actor::on_trigger);
        }

        int on_bump() { return 1; }

        void on_trigger(const increment_msg&) {
            try {
                self().ask<int>(bump_op{});
            } catch (const cas::ask_deadlock_error&) {
                threw.store(true);
            }
            completed.store(true);
        }
    };

    auto a = cas::system::create<self_ask_actor>();
    cas::system::start();
    wait_ms(50);

    auto ref = cas::actor_registry::get("self_ask_actor");
    REQUIRE(ref.is_valid());
    ref.tell(increment_msg{});

    // Watchdog: if the handler hangs, fail rather than blocking the suite
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    auto& actor_obj = a.get_checked<self_ask_actor>();
    while (!actor_obj.completed.load() &&
           std::chrono::steady_clock::now() < deadline) {
        wait_ms(10);
    }

    REQUIRE(actor_obj.completed.load());  // did not hang
    REQUIRE(actor_obj.threw.load());      // threw the right error

    TEST_CLEANUP();
}
