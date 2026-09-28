#include "../test_common.h"
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

// Level 7: the watch pattern - system::watch() / unwatch() and termination_msg.
//
// The watch table used to hold raw actor pointers, keyed by the watched actor's
// address (issue 006):
//  - a watcher destroyed before the actor it watched was dereferenced when that
//    actor stopped (use-after-free);
//  - a watcher that had stopped, but was still referenced, was sent a
//    termination_msg that immediately became a spurious dead letter;
//  - reset() left the table populated, so entries from one run could fire into
//    the next;
//  - entries for watchers that stopped were never removed.

namespace watch_test {

struct record {
    std::string watcher;
    std::string terminated;
    size_t instance_id;
};

std::mutex g_mutex;
std::vector<record> g_received;

void clear_received() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_received.clear();
}

std::vector<record> received() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_received;
}

class watcher_actor : public cas::actor {
public:
    explicit watcher_actor(std::string label) : m_label(std::move(label)) {}

protected:
    void on_start() override {
        handler<cas::termination_msg>([this](const cas::termination_msg& msg) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_received.push_back({m_label, msg.actor_name, msg.instance_id});
        });
    }

private:
    std::string m_label;
};

class plain_actor : public cas::actor {
public:
    explicit plain_actor(std::string name) : m_name(std::move(name)) {}

protected:
    void on_start() override { set_name(m_name); }

private:
    std::string m_name;
};

bool wait_for_count(size_t n, int timeout_ms = 3000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (received().size() >= n) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return received().size() >= n;
}

// Long enough for a notification that should NOT arrive to have arrived.
void settle() { std::this_thread::sleep_for(std::chrono::milliseconds(100)); }

} // namespace watch_test

using namespace watch_test;

TEST_CASE("A watcher is told once when the watched actor stops", "[07_dynamic_removal][watch]") {
    CAS_TEST_GUARD();
    clear_received();
    auto w = cas::system::create<watcher_actor>("w");
    auto target = cas::system::create<plain_actor>("target");
    cas::system::start();

    cas::system::watch(w, target);
    const size_t target_id = target.get<cas::actor>()->instance_id();
    REQUIRE(cas::system::stop_actor(target));

    REQUIRE(wait_for_count(1));
    settle();
    auto got = received();
    REQUIRE(got.size() == 1);
    REQUIRE(got[0].watcher == "w");
    REQUIRE(got[0].terminated == "target");
    REQUIRE(got[0].instance_id == target_id);
}

TEST_CASE("Several watchers of one actor, and one watcher of several actors", "[07_dynamic_removal][watch]") {
    CAS_TEST_GUARD();
    clear_received();
    auto w1 = cas::system::create<watcher_actor>("w1");
    auto w2 = cas::system::create<watcher_actor>("w2");
    auto a = cas::system::create<plain_actor>("a");
    auto b = cas::system::create<plain_actor>("b");
    cas::system::start();

    cas::system::watch(w1, a);
    cas::system::watch(w2, a);
    cas::system::watch(w1, b);
    cas::system::watch(w1, a);  // duplicate watch: still one notification

    REQUIRE(cas::system::stop_actor(a));
    REQUIRE(cas::system::stop_actor(b));

    REQUIRE(wait_for_count(3));
    settle();
    auto got = received();
    REQUIRE(got.size() == 3);
    int w1_a = 0, w2_a = 0, w1_b = 0;
    for (const auto& r : got) {
        if (r.watcher == "w1" && r.terminated == "a") ++w1_a;
        if (r.watcher == "w2" && r.terminated == "a") ++w2_a;
        if (r.watcher == "w1" && r.terminated == "b") ++w1_b;
    }
    REQUIRE(w1_a == 1);
    REQUIRE(w2_a == 1);
    REQUIRE(w1_b == 1);
}

TEST_CASE("unwatch stops the notification", "[07_dynamic_removal][watch]") {
    CAS_TEST_GUARD();
    clear_received();
    auto w = cas::system::create<watcher_actor>("w");
    auto target = cas::system::create<plain_actor>("target");
    cas::system::start();

    cas::system::watch(w, target);
    cas::system::unwatch(w, target);
    REQUIRE(cas::system::stop_actor(target));

    settle();
    REQUIRE(received().empty());
}

TEST_CASE("A watcher destroyed before the watched actor stops is skipped safely", "[07_dynamic_removal][watch]") {
    CAS_TEST_GUARD();
    clear_received();
    cas::system::reset_dead_letter_stats();
    auto target = cas::system::create<plain_actor>("target");
    cas::system::start();

    {
        auto w = cas::system::create<watcher_actor>("gone");
        cas::system::watch(w, target);
        REQUIRE(cas::system::stop_actor(w));
    }   // last reference released: the watcher is destroyed

    // Used to dereference the destroyed watcher here.
    REQUIRE(cas::system::stop_actor(target));

    settle();
    REQUIRE(received().empty());
    REQUIRE(cas::system::get_dead_letter_stats().dropped_tell == 0);
}

TEST_CASE("A watcher that has stopped is not sent a notification", "[07_dynamic_removal][watch]") {
    CAS_TEST_GUARD();
    clear_received();
    cas::system::reset_dead_letter_stats();
    auto w = cas::system::create<watcher_actor>("stopped");
    auto target = cas::system::create<plain_actor>("target");
    cas::system::start();

    cas::system::watch(w, target);
    REQUIRE(cas::system::stop_actor(w));  // still referenced by `w`
    REQUIRE(cas::system::stop_actor(target));

    settle();
    REQUIRE(received().empty());
    // Used to be sent anyway, and dropped as a dead letter.
    REQUIRE(cas::system::get_dead_letter_stats().dropped_tell == 0);
}

TEST_CASE("reset() forgets watches from the previous run", "[07_dynamic_removal][watch]") {
    CAS_TEST_GUARD();
    clear_received();

    // Run 1: w watches t, and the system shuts down without stopping t, so the
    // watch is never consumed.
    {
        auto w = cas::system::create<watcher_actor>("old-watcher");
        auto t = cas::system::create<plain_actor>("old-target");
        cas::system::start();
        cas::system::watch(w, t);
        cas::system::shutdown();
        cas::system::wait_for_shutdown();
    }
    cas::system::reset();

    // Run 2: same creation order, so the new actors get the same instance ids
    // (reset() restarts the counter) and may even reuse the same memory.
    cas::system::reset_dead_letter_stats();
    auto w2 = cas::system::create<watcher_actor>("new-watcher");
    auto t2 = cas::system::create<plain_actor>("new-target");
    cas::system::start();
    REQUIRE(cas::system::stop_actor(t2));

    settle();
    // Nobody watches t2 in this run.
    REQUIRE(received().empty());
    REQUIRE(cas::system::get_dead_letter_stats().dropped_tell == 0);
}

TEST_CASE("Stopped watchers do not accumulate in the watch table", "[07_dynamic_removal][watch]") {
    CAS_TEST_GUARD();
    clear_received();
    auto target = cas::system::create<plain_actor>("long-lived");
    cas::system::start();

    // Many short-lived watchers of one long-lived actor.
    for (int i = 0; i < 200; ++i) {
        auto w = cas::system::create<watcher_actor>("w" + std::to_string(i));
        cas::system::watch(w, target);
        REQUIRE(cas::system::stop_actor(w));
    }

    // One live watcher at the end: the only one that should hear about it.
    auto last = cas::system::create<watcher_actor>("last");
    cas::system::watch(last, target);
    REQUIRE(cas::system::stop_actor(target));

    REQUIRE(wait_for_count(1));
    settle();
    auto got = received();
    REQUIRE(got.size() == 1);
    REQUIRE(got[0].watcher == "last");
}
