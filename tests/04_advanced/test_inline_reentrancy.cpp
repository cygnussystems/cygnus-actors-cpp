#include "../test_common.h"
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

// Issue 004: inline actors re-entered from their own handlers, and call()
// deadlock detection decided by thread rather than by actor affinity.
//
// - A handler that tells its own inline actor (directly, or through a chain
//   of inline actors) used to deadlock on the non-recursive mutex, or nest
//   dispatch. Such sends are now handled after the current handler returns,
//   in send order.
// - A re-entrant call() into an inline actor can never be answered: it
//   throws call_deadlock_error.
// - call() used to compare the calling actor's worker affinity with the
//   target's. An inline actor has a worker affinity but runs on its sender's
//   thread, so the check was wrong both ways.

namespace inline_reentry_test {

    struct start : public cas::message_base {
        int steps = 3;
    };
    struct step : public cas::message_base {
        int n = 0;
    };

    struct get_op {};
    struct ping_op {};

    template<bool ThreadSafe>
    class self_teller : public cas::inline_actor<ThreadSafe> {
    public:
        std::mutex log_mutex;
        std::vector<std::string> log;
        std::atomic<int> depth{0};
        std::atomic<int> max_depth{0};

        std::vector<std::string> snapshot() {
            std::lock_guard<std::mutex> lock(log_mutex);
            return log;
        }

    protected:
        void on_start() override {
            this->template handler<start>(&self_teller::on_begin);
            this->template handler<step>(&self_teller::on_step);
        }

        void record(const std::string& entry) {
            int d = ++depth;
            int m = max_depth.load();
            while (d > m && !max_depth.compare_exchange_weak(m, d)) {
            }
            std::lock_guard<std::mutex> lock(log_mutex);
            log.push_back(entry);
        }

        void on_begin(const start& msg) {
            record("start");
            for (int i = 1; i <= msg.steps; ++i) {
                step s;
                s.n = i;
                this->self().tell(s);
            }
            // Still inside this handler: nothing told above has run yet
            std::lock_guard<std::mutex> lock(log_mutex);
            log.push_back("start-end");
            --depth;
        }

        void on_step(const step& msg) {
            record("step" + std::to_string(msg.n));
            --depth;
        }
    };

    template<bool ThreadSafe>
    void check_self_tell() {
        auto ref = cas::system::create<self_teller<ThreadSafe>>();
        cas::system::start();

        ref.tell(start{});  // inline: handled, with its self-sends, before this returns

        auto& a = ref.template get_checked<self_teller<ThreadSafe>>();
        auto log = a.snapshot();
        REQUIRE(log == std::vector<std::string>{"start", "start-end", "step1", "step2", "step3"});
        REQUIRE(a.max_depth.load() == 1);  // never nested
    }

    // Inline actor whose handler calls itself through call()
    class self_caller : public cas::inline_actor<> {
    public:
        std::atomic<bool> threw{false};
    protected:
        void on_start() override {
            handler<start>([this](const start&) {
                try {
                    self().call<int>(get_op{});
                } catch (const cas::call_deadlock_error&) {
                    threw = true;
                }
            });
            call_handler<int, get_op>(&self_caller::get);
        }
        int get() { return 1; }
    };

    // a -> tell b -> b's handler calls a, which is still handling on this thread
    class cycle_b;
    class cycle_a : public cas::inline_actor<> {
    public:
        cas::actor_ref peer;
    protected:
        void on_start() override {
            handler<start>([this](const start&) { peer.tell(step{}); });
            call_handler<int, ping_op>(&cycle_a::ping);
        }
        int ping() { return 7; }
    };
    class cycle_b : public cas::inline_actor<> {
    public:
        cas::actor_ref peer;
        std::atomic<bool> threw{false};
    protected:
        void on_start() override {
            handler<step>([this](const step&) {
                try {
                    peer.call<int>(ping_op{});
                } catch (const cas::call_deadlock_error&) {
                    threw = true;
                }
            });
        }
    };

    // Answers call()s; lives on a worker
    class pooled_service : public cas::actor {
    protected:
        void on_start() override {
            call_handler<int, get_op>(&pooled_service::get);
        }
        int get() { return 42; }
    };

    class inline_service : public cas::inline_actor<> {
    public:
        cas::actor_ref pooled;
        std::atomic<int> result{0};
        std::atomic<bool> threw{false};
    protected:
        void on_start() override {
            call_handler<int, get_op>(&inline_service::get);
            // Runs on whatever thread told it: calling a pooled actor that
            // only that (busy) worker executes would never be answered.
            handler<start>([this](const start&) {
                try {
                    result = pooled.call<int>(get_op{});
                } catch (const cas::call_deadlock_error&) {
                    threw = true;
                }
            });
        }
        int get() { return 5; }
    };

    // Pooled actor on the same (only) worker as everything else
    class pooled_caller : public cas::actor {
    public:
        cas::actor_ref target;
        std::atomic<int> result{0};
        std::atomic<bool> threw{false};
        std::atomic<bool> done{false};
    protected:
        void on_start() override {
            handler<start>([this](const start&) {
                try {
                    result = target.call<int>(get_op{});
                } catch (const cas::call_deadlock_error&) {
                    threw = true;
                }
                done = true;
            });
            handler<step>([this](const step&) {
                target.tell(start{});  // target is inline: runs here, on this worker
                done = true;
            });
        }
    };

    class fast_caller : public cas::fast_actor {
    public:
        cas::actor_ref target;
        std::atomic<int> result{0};
        std::atomic<bool> threw{false};
        std::atomic<bool> done{false};
    protected:
        void on_start() override {
            handler<start>([this](const start&) {
                try {
                    result = target.call<int>(get_op{});
                } catch (const cas::call_deadlock_error&) {
                    threw = true;
                }
                done = true;
            });
        }
    };

    struct boom : public cas::message_base {};

    class thrower : public cas::inline_actor<> {
    public:
        std::atomic<int> steps{0};
    protected:
        void on_start() override {
            handler<boom>([this](const boom&) {
                self().tell(step{});  // deferred behind this handler...
                throw std::runtime_error("handler failed");
            });
            handler<step>([this](const step&) { steps++; });
        }
    };

    template<typename Pred>
    bool wait_for(Pred pred) {
        for (int i = 0; i < 400 && !pred(); ++i) {
            wait_ms(5);
        }
        return pred();
    }

    void single_worker() {
        cas::system_config cfg;
        cfg.thread_pool_size = 1;
        cas::system::configure(cfg);
    }
}

using namespace inline_reentry_test;

TEST_CASE("Inline actor telling itself runs the messages after its handler (thread-safe)",
          "[04_advanced][inline_actor][issue004]") {
    CAS_TEST_GUARD();
    check_self_tell<true>();
}

TEST_CASE("Inline actor telling itself runs the messages after its handler (single sender)",
          "[04_advanced][inline_actor][issue004]") {
    CAS_TEST_GUARD();
    check_self_tell<false>();
}

TEST_CASE("Inline actor calling itself throws call_deadlock_error",
          "[04_advanced][inline_actor][issue004]") {
    CAS_TEST_GUARD();
    auto ref = cas::system::create<self_caller>();
    cas::system::start();
    ref.tell(start{});
    REQUIRE(ref.get_checked<self_caller>().threw.load());
}

TEST_CASE("call() back into an inline actor that is mid-handler throws",
          "[04_advanced][inline_actor][issue004]") {
    CAS_TEST_GUARD();
    auto a = cas::system::create<cycle_a>();
    auto b = cas::system::create<cycle_b>();
    a.get_checked<cycle_a>().peer = b;
    b.get_checked<cycle_b>().peer = a;
    cas::system::start();

    a.tell(start{});
    REQUIRE(b.get_checked<cycle_b>().threw.load());

    // Not mid-handler any more: the same call now succeeds
    REQUIRE(a.call<int>(ping_op{}) == 7);
}

TEST_CASE("A pooled actor may call an inline actor on its own worker",
          "[04_advanced][inline_actor][issue004]") {
    CAS_CONFIG_GUARD();
    single_worker();
    auto target = cas::system::create<inline_service>();
    auto caller = cas::system::create<pooled_caller>();
    caller.get_checked<pooled_caller>().target = target;
    cas::system::start();

    caller.tell(start{});
    auto& c = caller.get_checked<pooled_caller>();
    REQUIRE(wait_for([&] { return c.done.load(); }));
    REQUIRE_FALSE(c.threw.load());
    REQUIRE(c.result.load() == 5);
}

TEST_CASE("An inline handler calling a pooled actor on its sender's busy worker throws",
          "[04_advanced][inline_actor][issue004]") {
    CAS_CONFIG_GUARD();
    single_worker();
    auto service = cas::system::create<pooled_service>();
    auto middle = cas::system::create<inline_service>();
    auto caller = cas::system::create<pooled_caller>();
    middle.get_checked<inline_service>().pooled = service;
    caller.get_checked<pooled_caller>().target = middle;
    cas::system::start();

    // pooled_caller (worker 0) tells the inline actor, whose handler runs on
    // worker 0 and calls pooled_service - also worker 0, which is busy.
    caller.tell(step{});
    auto& c = caller.get_checked<pooled_caller>();
    REQUIRE(wait_for([&] { return c.done.load(); }));
    REQUIRE(middle.get_checked<inline_service>().threw.load());

    // From a thread that is not the worker, the same call works
    middle.tell(start{});
    REQUIRE(middle.get_checked<inline_service>().result.load() == 42);
}

TEST_CASE("A fast actor may call a pooled actor on any worker",
          "[04_advanced][inline_actor][issue004]") {
    CAS_CONFIG_GUARD();
    single_worker();
    auto service = cas::system::create<pooled_service>();
    auto caller = cas::system::create<fast_caller>();
    caller.get_checked<fast_caller>().target = service;
    cas::system::start();

    caller.tell(start{});
    auto& c = caller.get_checked<fast_caller>();
    REQUIRE(wait_for([&] { return c.done.load(); }));
    REQUIRE_FALSE(c.threw.load());
    REQUIRE(c.result.load() == 42);
}

TEST_CASE("An inline handler exception reaches the sender and leaves the actor usable",
          "[04_advanced][inline_actor][issue004]") {
    CAS_TEST_GUARD();
    cas::system::reset_dead_letter_stats();
    auto ref = cas::system::create<thrower>();
    cas::system::start();

    REQUIRE_THROWS_AS(ref.tell(boom{}), std::runtime_error);
    auto& t = ref.get_checked<thrower>();
    REQUIRE(t.steps.load() == 0);  // the deferred message was dropped...
    REQUIRE(cas::system::get_dead_letter_stats().dropped_tell == 1);  // ...as a dead letter

    ref.tell(step{});  // not stuck in "re-entrant" mode
    REQUIRE(t.steps.load() == 1);
}
