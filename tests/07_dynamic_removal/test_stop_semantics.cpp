#include "../test_common.h"
#include "cas/cas.h"
#include <atomic>
#include <thread>
#include <chrono>

// Level 7: what stop_actor() guarantees
//
// stop_actor() used to run on_shutdown()/on_stop() on the CALLER's thread
// while the actor's worker could still be running a handler - an INVARIANT 1
// violation invisible to dispatch_guard, which only watches dispatch. The
// same code made stop_mode::discard leave the queue to be processed anyway,
// made wait_for_stop=false still run everything synchronously, and called a
// fast actor's on_stop() twice. The stop now runs on the actor's own thread.

using namespace std::chrono_literals;

namespace stop_semantics_test {

struct work : cas::message_base {
    int sleep_ms = 0;
};

struct stop_me : cas::message_base {};

std::atomic<int> g_handled{0};
std::atomic<bool> g_in_handler{false};
std::atomic<bool> g_overlap{false};          // a hook ran while a handler was running
std::atomic<bool> g_handler_after_stop{false};
std::atomic<int> g_on_shutdown_calls{0};
std::atomic<int> g_on_stop_calls{0};
std::atomic<bool> g_stop_done{false};
std::atomic<bool> g_hook_off_thread{false};  // a hook ran on a thread no handler used

void reset_globals() {
    g_handled = 0;
    g_in_handler = false;
    g_overlap = false;
    g_handler_after_stop = false;
    g_on_shutdown_calls = 0;
    g_on_stop_calls = 0;
    g_stop_done = false;
    g_hook_off_thread = false;
}

// Records overlap between handlers and hooks, and which thread runs each.
template<typename Base>
class probe_base : public Base {
protected:
    void on_start() override {
        this->template handler<work>(&probe_base::on_work);
        this->template handler<stop_me>(&probe_base::on_stop_me);
    }

    void on_work(const work& msg) {
        if (g_stop_done.load()) g_handler_after_stop = true;
        m_handler_thread = std::this_thread::get_id();
        g_in_handler = true;
        if (msg.sleep_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(msg.sleep_ms));
        g_in_handler = false;
        g_handled.fetch_add(1);
    }

    void on_stop_me(const stop_me&) {
        m_handler_thread = std::this_thread::get_id();
        cas::system::stop_actor(this->self());  // self-stop must not deadlock
    }

    void on_shutdown() override {
        check_hook();
        g_on_shutdown_calls.fetch_add(1);
    }

    void on_stop() override {
        check_hook();
        g_on_stop_calls.fetch_add(1);
        g_stop_done = true;
    }

private:
    void check_hook() {
        if (g_in_handler.load()) g_overlap = true;
        if (m_handler_thread != std::thread::id{} &&
            m_handler_thread != std::this_thread::get_id()) {
            g_hook_off_thread = true;
        }
    }

    // Written by handlers and read by hooks: both on the executing thread
    // once the fix is in place, which is exactly what is being tested.
    std::thread::id m_handler_thread{};
};

using pooled_probe = probe_base<cas::actor>;
using fast_probe = probe_base<cas::fast_actor>;

// on_stop() calling back into the system - deadlocked when stop_actor()
// joined the fast actor's thread while holding m_fast_actors_mutex.
std::atomic<size_t> g_count_from_on_stop{0};

class counting_fast_actor : public cas::fast_actor {
protected:
    void on_start() override {}
    void on_stop() override {
        g_count_from_on_stop = cas::system::actor_count();
    }
};

class noop_actor : public cas::actor {
protected:
    void on_start() override {}
};

// Asks another actor on the same worker thread to stop it, blocking.
struct stop_target : cas::message_base {
    cas::actor_ref target;
};

class stopper : public cas::actor {
protected:
    void on_start() override {
        handler<stop_target>([](const stop_target& msg) {
            cas::system::stop_actor(msg.target);
            g_stop_done = true;
        });
    }
};

template<typename Pred>
bool wait_for(Pred pred, int timeout_ms = 3000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return pred();
}

} // namespace stop_semantics_test

using namespace stop_semantics_test;

TEST_CASE("stop_actor runs hooks on the actor's thread, after the running handler", "[07_dynamic_removal][stop]") {
    CAS_TEST_GUARD();
    reset_globals();
    auto ref = cas::system::create<pooled_probe>();
    cas::system::start();

    work slow;
    slow.sleep_ms = 150;
    ref.tell(slow);
    REQUIRE(wait_for([] { return g_in_handler.load(); }));

    // Discard skips the drain, so the old code ran on_stop() at once, in the
    // middle of the 150ms handler.
    cas::stop_config cfg;
    cfg.mode = cas::stop_mode::discard;
    REQUIRE(cas::system::stop_actor(ref, cfg));

    // Blocking stop: hooks have completed when it returns
    REQUIRE(g_on_stop_calls.load() == 1);
    REQUIRE(g_on_shutdown_calls.load() == 1);
    REQUIRE_FALSE(g_overlap.load());
    REQUIRE_FALSE(g_hook_off_thread.load());
    REQUIRE(g_handled.load() == 1);
}

TEST_CASE("stop_mode::discard drops queued messages as dead letters", "[07_dynamic_removal][stop]") {
    CAS_TEST_GUARD();
    reset_globals();
    cas::system::reset_dead_letter_stats();
    auto ref = cas::system::create<pooled_probe>();
    cas::system::start();

    work slow;
    slow.sleep_ms = 50;
    for (int i = 0; i < 10; ++i) ref.tell(slow);
    REQUIRE(wait_for([] { return g_in_handler.load(); }));

    cas::stop_config cfg;
    cfg.mode = cas::stop_mode::discard;
    REQUIRE(cas::system::stop_actor(ref, cfg));

    const int handled = g_handled.load();
    const size_t discarded = cas::system::get_dead_letter_stats().dropped_tell;
    CHECK(handled < 10);
    CHECK(static_cast<size_t>(handled) + discarded == 10);

    // Nothing left runs after the stop
    std::this_thread::sleep_for(150ms);
    REQUIRE(g_handled.load() == handled);
    REQUIRE_FALSE(g_handler_after_stop.load());
}

TEST_CASE("stop_mode::drain processes queued messages before on_stop", "[07_dynamic_removal][stop]") {
    CAS_TEST_GUARD();
    reset_globals();
    auto ref = cas::system::create<pooled_probe>();
    cas::system::start();

    work w;
    w.sleep_ms = 5;
    for (int i = 0; i < 10; ++i) ref.tell(w);

    REQUIRE(cas::system::stop_actor(ref));  // default: drain, wait

    REQUIRE(g_handled.load() == 10);
    REQUIRE(g_on_stop_calls.load() == 1);
    REQUIRE_FALSE(g_overlap.load());
    REQUIRE_FALSE(g_handler_after_stop.load());
}

TEST_CASE("wait_for_stop=false returns before the stop completes", "[07_dynamic_removal][stop]") {
    CAS_TEST_GUARD();
    reset_globals();
    auto ref = cas::system::create<pooled_probe>();
    cas::system::start();

    work slow;
    slow.sleep_ms = 150;
    ref.tell(slow);
    REQUIRE(wait_for([] { return g_in_handler.load(); }));

    cas::stop_config cfg;
    cfg.wait_for_stop = false;
    REQUIRE(cas::system::stop_actor(ref, cfg));

    // Returned while the handler is still running; already not accepting messages
    REQUIRE_FALSE(g_stop_done.load());
    REQUIRE_FALSE(ref.is_running());

    REQUIRE(wait_for([] { return g_stop_done.load(); }));
    REQUIRE(g_on_stop_calls.load() == 1);
    REQUIRE_FALSE(g_overlap.load());
}

TEST_CASE("An actor can stop itself from a handler", "[07_dynamic_removal][stop]") {
    CAS_TEST_GUARD();
    reset_globals();
    auto ref = cas::system::create<pooled_probe>();
    cas::system::start();

    ref.tell(stop_me{});

    REQUIRE(wait_for([] { return g_stop_done.load(); }));
    REQUIRE(g_on_stop_calls.load() == 1);
    REQUIRE_FALSE(g_hook_off_thread.load());
    REQUIRE_FALSE(ref.is_running());
}

TEST_CASE("Stopping an actor that shares the caller's worker thread does not deadlock", "[07_dynamic_removal][stop]") {
    CAS_CONFIG_GUARD();
    reset_globals();
    cas::system_config config;
    config.thread_pool_size = 1;  // both actors on the same worker
    cas::system::configure(config);

    auto target = cas::system::create<noop_actor>();
    auto caller = cas::system::create<stopper>();
    cas::system::start();

    stop_target msg;
    msg.target = target;
    caller.tell(msg);

    REQUIRE(wait_for([] { return g_stop_done.load(); }));
    REQUIRE(wait_for([&] { return cas::system::actor_count() == 1; }));
}

TEST_CASE("A fast actor's on_stop runs once, on its own thread", "[07_dynamic_removal][stop][fast]") {
    CAS_TEST_GUARD();
    reset_globals();
    auto ref = cas::system::create<fast_probe>();
    cas::system::start();

    work w;
    ref.tell(w);
    REQUIRE(wait_for([] { return g_handled.load() == 1; }));

    REQUIRE(cas::system::stop_actor(ref));

    REQUIRE(g_on_stop_calls.load() == 1);
    REQUIRE(g_on_shutdown_calls.load() == 1);
    REQUIRE_FALSE(g_hook_off_thread.load());
    REQUIRE(cas::system::actor_count() == 0);

    // Shutdown must not run its hooks again
    cas::system::shutdown();
    cas::system::wait_for_shutdown();
    REQUIRE(g_on_stop_calls.load() == 1);
}

TEST_CASE("A fast actor can stop itself from a handler", "[07_dynamic_removal][stop][fast]") {
    CAS_TEST_GUARD();
    reset_globals();
    auto ref = cas::system::create<fast_probe>();
    cas::system::start();

    ref.tell(stop_me{});

    REQUIRE(wait_for([] { return g_stop_done.load(); }));
    REQUIRE(g_on_stop_calls.load() == 1);
    REQUIRE(wait_for([] { return cas::system::actor_count() == 0; }));
}

TEST_CASE("A fast actor's on_stop may call back into the system", "[07_dynamic_removal][stop][fast]") {
    CAS_TEST_GUARD();
    g_count_from_on_stop = 999;
    auto fast = cas::system::create<counting_fast_actor>();
    auto other = cas::system::create<noop_actor>();
    cas::system::start();

    // Previously deadlocked: stop_actor() held m_fast_actors_mutex across
    // join() while on_stop() waited for it inside actor_count().
    REQUIRE(cas::system::stop_actor(fast));
    REQUIRE(g_count_from_on_stop.load() != 999);
}

TEST_CASE("stop_actor before start completes on the caller's thread", "[07_dynamic_removal][stop]") {
    CAS_TEST_GUARD();
    reset_globals();
    auto ref = cas::system::create<pooled_probe>();

    REQUIRE(cas::system::stop_actor(ref));
    REQUIRE(g_on_stop_calls.load() == 1);
    REQUIRE(cas::system::actor_count() == 0);
}

TEST_CASE("A stop requested during shutdown is completed by wait_for_shutdown", "[07_dynamic_removal][stop]") {
    CAS_TEST_GUARD();
    reset_globals();
    auto ref = cas::system::create<pooled_probe>();
    cas::system::start();

    work slow;
    slow.sleep_ms = 100;
    ref.tell(slow);
    REQUIRE(wait_for([] { return g_in_handler.load(); }));

    cas::stop_config cfg;
    cfg.wait_for_stop = false;
    REQUIRE(cas::system::stop_actor(ref, cfg));

    cas::system::shutdown();
    cas::system::wait_for_shutdown();

    // Exactly once, either on the worker or in wait_for_shutdown - never both
    REQUIRE(g_on_stop_calls.load() == 1);
    REQUIRE(g_on_shutdown_calls.load() == 1);
    REQUIRE_FALSE(g_overlap.load());
}
