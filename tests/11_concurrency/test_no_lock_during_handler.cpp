#include "../test_common.h"
#include <atomic>

// Level 11: INVARIANT 2 - no framework lock is held while user code runs.
//
// The original implementation held the single system-wide m_actors_mutex across
// process_next_message(). Any handler that created an actor re-entered
// register_actor(), which takes the same non-recursive mutex, and self-deadlocked.
// No test in the original suite created an actor from inside a handler, so the
// deadlock was invisible.

namespace lock_scope_test {

    struct spawn_msg : public cas::message_base {};
    struct ping_child : public cas::message_base {};

    class child_actor : public cas::actor {
    protected:
        void on_start() override {}
    };

    class spawner_actor : public cas::actor {
    public:
        std::atomic<bool> spawn_returned{false};
        std::atomic<bool> child_valid{false};

    protected:
        void on_start() override {
            set_name("spawner_actor");
            handler<spawn_msg>(&spawner_actor::on_spawn);
        }

        void on_spawn(const spawn_msg&) {
            // Creating an actor from inside a handler re-enters register_actor().
            // If the worker still held m_actors_mutex here, this would deadlock.
            // Note: on_start() is only invoked by system::start(), so an actor
            // created at runtime is registered but not started - that is a
            // separate framework limitation, not what this test is about.
            auto child = cas::system::create<child_actor>();
            child_valid.store(child.is_valid());
            spawn_returned.store(true);
        }
    };
}

TEST_CASE("Handler can create an actor without deadlocking", "[11_concurrency][invariant2]") {
    using namespace lock_scope_test;

    auto spawner = cas::system::create<spawner_actor>();
    cas::system::start();
    wait_ms(50);

    auto ref = cas::actor_registry::get("spawner_actor");
    REQUIRE(ref.is_valid());
    ref.tell(spawn_msg{});

    // Watchdog. A deadlock here would otherwise hang the whole suite with no
    // diagnostic, so bound the wait and fail loudly instead.
    auto& actor_obj = spawner.get_checked<spawner_actor>();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!actor_obj.spawn_returned.load() &&
           std::chrono::steady_clock::now() < deadline) {
        wait_ms(10);
    }

    // The point of the test: the handler returned instead of deadlocking.
    REQUIRE(actor_obj.spawn_returned.load());
    REQUIRE(actor_obj.child_valid.load());

    TEST_CLEANUP();
}

TEST_CASE("A slow handler does not block other worker threads", "[11_concurrency][invariant2]") {
    // With the system-wide mutex held across dispatch, one slow handler stalled
    // every worker. Two actors on different threads should make progress
    // independently.
    struct slow_msg : public cas::message_base {};
    struct fast_msg : public cas::message_base {};

    struct slow_actor : public cas::actor {
        std::atomic<bool> done{false};
        void on_start() override {
            set_name("slow_one");
            handler<slow_msg>([this](const slow_msg&) {
                std::this_thread::sleep_for(std::chrono::milliseconds(600));
                done.store(true);
            });
        }
    };

    struct fast_actor_t : public cas::actor {
        std::atomic<int> count{0};
        void on_start() override {
            set_name("fast_one");
            handler<fast_msg>([this](const fast_msg&) { count.fetch_add(1); });
        }
    };

    auto slow = cas::system::create<slow_actor>();
    auto fast = cas::system::create<fast_actor_t>();
    cas::system::start();
    wait_ms(50);

    auto slow_ref = cas::actor_registry::get("slow_one");
    auto fast_ref = cas::actor_registry::get("fast_one");
    REQUIRE(slow_ref.is_valid());
    REQUIRE(fast_ref.is_valid());

    // Only meaningful if the two actors really landed on different threads.
    // Round-robin assignment guarantees this whenever the pool has >1 thread.
    size_t slow_thread = slow.get<cas::actor>()->get_thread_affinity();
    size_t fast_thread = fast.get<cas::actor>()->get_thread_affinity();
    if (slow_thread == fast_thread) {
        WARN("Single-threaded pool - cross-thread independence not exercised");
        TEST_CLEANUP();
        return;
    }

    slow_ref.tell(slow_msg{});
    wait_ms(50);  // ensure the slow handler is running

    for (int i = 0; i < 20; ++i) {
        fast_ref.tell(fast_msg{});
    }

    // The fast actor should drain well before the slow handler finishes
    wait_ms(250);
    auto& fast_obj = fast.get_checked<fast_actor_t>();
    auto& slow_obj = slow.get_checked<slow_actor>();

    REQUIRE(fast_obj.count.load() == 20);
    REQUIRE_FALSE(slow_obj.done.load());  // slow one is still working

    TEST_CLEANUP();
}
