#include "../test_common.h"
#include <atomic>

// Level 11: the on_start() contract.
//
// on_start() is for initialising the actor itself. What is permitted:
//   - set_name(), handler<>() / ask_handler<>() registration
//   - member initialisation
//   - schedule_once() / schedule_periodic()
//   - tell() to other actors (queued, delivered once workers run)
//   - system::create() (safe on both the startup and runtime paths)
//
// What is rejected:
//   - ask() - blocks waiting for a reply from a system that may not be
//     processing messages yet. At startup no worker thread exists, so this
//     would hang forever rather than fail.

namespace on_start_contract_test {

    struct probe_msg : public cas::message_base { int value = 0; };
    struct compute_op {};

    class target_actor : public cas::actor {
    public:
        std::atomic<int> received{0};
    protected:
        void on_start() override {
            set_name("contract_target");
            ask_handler<int, compute_op>(&target_actor::compute);
            handler<probe_msg>([this](const probe_msg&) { received.fetch_add(1); });
        }
        int compute() { return 99; }
    };
}

TEST_CASE("ask() from on_start() throws instead of hanging",
          "[11_concurrency][on_start_contract]") {
    CAS_TEST_GUARD();
    using namespace on_start_contract_test;

    class asking_actor : public cas::actor {
    public:
        std::atomic<bool> threw{false};
        std::atomic<bool> finished{false};
    protected:
        void on_start() override {
            set_name("asking_actor");
            auto target = cas::actor_registry::get("contract_target");
            if (target.is_valid()) {
                try {
                    target.ask<int>(compute_op{});
                } catch (const cas::on_start_violation&) {
                    threw.store(true);
                }
            }
            finished.store(true);
        }
    };

    auto target = cas::system::create<target_actor>();
    cas::system::start();
    wait_ms(50);

    // Created at runtime so the registry lookup for the target succeeds
    auto asker = cas::system::create<asking_actor>();
    auto& obj = asker.get_checked<asking_actor>();

    // create() runs on_start() synchronously - if ask() hung, we never get here
    REQUIRE(obj.finished.load());
    REQUIRE(obj.threw.load());

    TEST_CLEANUP();
}

TEST_CASE("tell() from on_start() is permitted and delivered",
          "[11_concurrency][on_start_contract]") {
    CAS_TEST_GUARD();
    using namespace on_start_contract_test;

    // This is the documented pattern in README.md and doc/120_best_practices.md.
    // It must keep working.
    class telling_actor : public cas::actor {
    protected:
        void on_start() override {
            set_name("telling_actor");
            auto target = cas::actor_registry::get("contract_target");
            if (target.is_valid()) {
                probe_msg msg;
                msg.value = 5;
                target.tell(msg);
            }
        }
    };

    auto target = cas::system::create<target_actor>();
    auto teller = cas::system::create<telling_actor>();

    cas::system::start();

    auto& tgt = target.get_checked<target_actor>();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (tgt.received.load() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        wait_ms(10);
    }

    REQUIRE(tgt.received.load() == 1);

    TEST_CLEANUP();
}

TEST_CASE("system::create() from on_start() does not deadlock at startup",
          "[11_concurrency][on_start_contract]") {
    CAS_TEST_GUARD();
    // start() used to hold m_actors_mutex across on_start(), so an actor that
    // created another actor during initialisation self-deadlocked. This is the
    // documented supervisor pattern (README.md, doc/70_dynamic_removal.md).
    struct child : public cas::actor {
        void on_start() override {}
    };

    struct supervisor : public cas::actor {
        cas::actor_ref worker;
        std::atomic<bool> created{false};

        void on_start() override {
            set_name("startup_supervisor");
            worker = cas::system::create<child>();
            created.store(true);
        }
    };

    auto sup = cas::system::create<supervisor>();

    // A deadlock here would hang the suite - Catch2 has no per-test timeout,
    // so this test failing means a hang rather than a clean assertion failure.
    cas::system::start();
    wait_ms(50);

    auto& obj = sup.get_checked<supervisor>();
    REQUIRE(obj.created.load());
    REQUIRE(obj.worker.is_valid());

    TEST_CLEANUP();
}

TEST_CASE("is_initialising() is false outside on_start()",
          "[11_concurrency][on_start_contract]") {
    CAS_TEST_GUARD();
    using namespace on_start_contract_test;

    // The flag must be cleared when on_start() returns, or ask() would be
    // rejected forever afterwards.
    class checker : public cas::actor {
    public:
        std::atomic<bool> init_during_start{false};
        std::atomic<bool> init_during_handler{true};
    protected:
        void on_start() override {
            set_name("init_checker");
            init_during_start.store(is_initialising());
            handler<probe_msg>([this](const probe_msg&) {
                init_during_handler.store(is_initialising());
            });
        }
    };

    auto c = cas::system::create<checker>();
    cas::system::start();
    wait_ms(50);

    auto ref = cas::actor_registry::get("init_checker");
    REQUIRE(ref.is_valid());
    ref.tell(probe_msg{});

    auto& obj = c.get_checked<checker>();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (obj.init_during_handler.load() &&
           std::chrono::steady_clock::now() < deadline) {
        wait_ms(10);
    }

    REQUIRE(obj.init_during_start.load());        // true inside on_start()
    REQUIRE_FALSE(obj.init_during_handler.load()); // false in a normal handler

    TEST_CLEANUP();
}
