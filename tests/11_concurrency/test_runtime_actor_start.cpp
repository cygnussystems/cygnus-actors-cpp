#include "../test_common.h"
#include <atomic>

// Level 11: actors created after system::start() are started too.
//
// on_start() used to be invoked only from system::start(), so an actor created
// at runtime was registered and assigned a thread but never started: its
// handlers were never registered and every message to it fell through to
// on_unhandled_message(). register_actor() now starts such actors itself.

namespace runtime_start_test {

    struct work_msg : public cas::message_base { int value = 0; };
    struct spawn_msg : public cas::message_base {};

    class late_actor : public cas::actor {
    public:
        std::atomic<int> received{0};
        std::atomic<int> last_value{0};
        std::atomic<bool> started{false};

    protected:
        void on_start() override {
            started.store(true);
            handler<work_msg>([this](const work_msg& msg) {
                last_value.store(msg.value);
                received.fetch_add(1);
            });
        }
    };
}

TEST_CASE("Actor created after start() is started and handles messages",
          "[11_concurrency][runtime_start]") {
    using namespace runtime_start_test;

    // Nothing is created before start() - the system comes up empty
    cas::system::start();
    wait_ms(50);

    auto late = cas::system::create<late_actor>();
    auto& obj = late.get_checked<late_actor>();

    // on_start() must have run during create()
    REQUIRE(obj.started.load());

    work_msg msg;
    msg.value = 42;
    late.tell(msg);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (obj.received.load() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        wait_ms(10);
    }

    // The handler registered in on_start() actually ran
    REQUIRE(obj.received.load() == 1);
    REQUIRE(obj.last_value.load() == 42);

    TEST_CLEANUP();
}

TEST_CASE("Actor spawned from inside a handler is started",
          "[11_concurrency][runtime_start]") {
    using namespace runtime_start_test;

    // Combines the runtime-start fix with INVARIANT 2: creating an actor from
    // inside a handler must neither deadlock nor produce an unstarted actor.
    struct spawner : public cas::actor {
        cas::actor_ref child;
        std::atomic<bool> spawned{false};

        void on_start() override {
            set_name("late_spawner");
            handler<spawn_msg>([this](const spawn_msg&) {
                child = cas::system::create<late_actor>();
                spawned.store(true);
            });
        }
    };

    auto sp = cas::system::create<spawner>();
    cas::system::start();
    wait_ms(50);

    auto ref = cas::actor_registry::get("late_spawner");
    REQUIRE(ref.is_valid());
    ref.tell(spawn_msg{});

    auto& sp_obj = sp.get_checked<spawner>();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!sp_obj.spawned.load() &&
           std::chrono::steady_clock::now() < deadline) {
        wait_ms(10);
    }
    REQUIRE(sp_obj.spawned.load());

    // The spawned child must be live, not merely registered
    REQUIRE(sp_obj.child.is_valid());
    auto& child_obj = sp_obj.child.get_checked<late_actor>();
    REQUIRE(child_obj.started.load());

    work_msg msg;
    msg.value = 7;
    sp_obj.child.tell(msg);

    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (child_obj.received.load() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        wait_ms(10);
    }
    REQUIRE(child_obj.received.load() == 1);
    REQUIRE(child_obj.last_value.load() == 7);

    TEST_CLEANUP();
}

TEST_CASE("Actors created before start() are started exactly once",
          "[11_concurrency][runtime_start]") {
    using namespace runtime_start_test;

    // Guards against the obvious regression: register_actor() must not start
    // pre-start actors, or start() would call on_start() a second time.
    struct counting_actor : public cas::actor {
        std::atomic<int> start_count{0};
        void on_start() override { start_count.fetch_add(1); }
    };

    auto a = cas::system::create<counting_actor>();
    auto& obj = a.get_checked<counting_actor>();

    // Not started yet - the system is not running
    REQUIRE(obj.start_count.load() == 0);

    cas::system::start();
    wait_ms(50);

    REQUIRE(obj.start_count.load() == 1);

    TEST_CLEANUP();
}
