#include "../test_common.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

// Level 11: INVARIANT 2 during system shutdown (issue 003).
//
// shutdown() and wait_for_shutdown() used to hold the system-wide actor-list
// mutexes while running user code: every pooled actor's on_stop(), an
// inline_actor's on_shutdown() (dispatched synchronously when shutdown()
// enqueues system_shutdown), and user callbacks fired by that enqueue (queue
// threshold, dead letters). User code that called back into the system - even
// just system::actor_count() - re-took the same non-recursive mutex and
// deadlocked the thread calling shutdown.
//
// That deadlock happens on the caller's thread, which cannot be recovered, so
// each call runs on a helper thread under a watchdog: if it does not finish,
// the test reports which call hung and ends the process (a hung shutdown would
// otherwise hang the whole suite with no diagnostic).

namespace shutdown_callbacks_test {

std::atomic<size_t> g_count_seen{0};
std::atomic<bool> g_callback_ran{false};

void run_or_die(const char* what, void (*fn)()) {
    std::atomic<bool> done{false};
    std::string error;
    std::thread t([&] {
        // MSVC's std::mutex detects a thread re-locking a mutex it holds and
        // throws system_error (resource_deadlock_would_occur) instead of
        // hanging. Catch it here: escaping the thread would terminate the
        // process with no diagnostic.
        try {
            fn();
        } catch (const std::exception& e) {
            error = e.what();
        }
        done = true;
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!done.load() || !error.empty()) {
        std::fprintf(stderr,
                     "\nDEADLOCK: %s %s - user code ran under a framework lock "
                     "and called back into the system (INVARIANT 2, issue 003).\n",
                     what, done.load() ? ("threw: " + error).c_str() : "did not return within 10s");
        std::fflush(stderr);
        std::_Exit(3);  // the system's locks may be held forever; cannot continue
    }
    t.join();
}

void full_shutdown() {
    cas::system::shutdown();
    cas::system::wait_for_shutdown();
}

void just_shutdown() {
    cas::system::shutdown();
}

class counts_in_on_stop : public cas::actor {
protected:
    void on_start() override {}
    void on_stop() override {
        g_count_seen = cas::system::actor_count();  // takes m_actors_mutex
        g_callback_ran = true;
    }
};

class inline_counts_in_on_shutdown : public cas::inline_actor<true> {
protected:
    void on_start() override {}
    void on_shutdown() override {
        g_count_seen = cas::system::actor_count();
        g_callback_ran = true;
    }
};

struct slow : cas::message_base {};

class slow_worker : public cas::actor {
protected:
    void on_start() override {
        handler<slow>([](const slow&) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        });
    }
};

} // namespace shutdown_callbacks_test

using namespace shutdown_callbacks_test;

TEST_CASE("on_stop may call back into the system during wait_for_shutdown",
          "[11_concurrency][invariant2][shutdown]") {
    CAS_TEST_GUARD();
    g_callback_ran = false;
    cas::system::create<counts_in_on_stop>();
    cas::system::create<counts_in_on_stop>();
    cas::system::start();

    run_or_die("wait_for_shutdown() with on_stop() calling actor_count()", &full_shutdown);

    REQUIRE(g_callback_ran.load());
    REQUIRE(g_count_seen.load() == 2);
}

TEST_CASE("An inline actor's on_shutdown may call back into the system",
          "[11_concurrency][invariant2][shutdown]") {
    CAS_TEST_GUARD();
    g_callback_ran = false;
    cas::system::create<inline_counts_in_on_shutdown>();
    cas::system::start();

    // shutdown() enqueues system_shutdown, which an inline actor dispatches
    // synchronously - on the thread calling shutdown().
    run_or_die("shutdown() with an inline actor's on_shutdown() calling actor_count()",
               &just_shutdown);

    REQUIRE(g_callback_ran.load());
    REQUIRE(g_count_seen.load() == 1);
}

TEST_CASE("A queue threshold callback fired by shutdown may call back into the system",
          "[11_concurrency][invariant2][shutdown]") {
    CAS_CONFIG_GUARD();
    cas::system_config config;
    config.queue_threshold = 1;
    cas::system::configure(config);

    g_callback_ran = false;
    cas::system::set_queue_threshold_handler([](const cas::queue_threshold_info&) {
        g_count_seen = cas::system::actor_count();
        g_callback_ran = true;
    });

    auto busy = cas::system::create<slow_worker>();
    cas::system::start();

    // One message is being handled (200ms), one waits: the mailbox holds 1,
    // which does not exceed the threshold. shutdown()'s system_shutdown makes
    // it 2 - the breach, reported on the thread calling shutdown().
    busy.tell(slow{});
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    busy.tell(slow{});

    run_or_die("shutdown() with a queue threshold callback calling actor_count()",
               &just_shutdown);

    REQUIRE(g_callback_ran.load());
    REQUIRE(g_count_seen.load() == 1);
}
