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
