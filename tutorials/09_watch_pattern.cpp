// Tutorial 9: Watching an actor for termination
//
// TEACHES
//   - system::watch() and the termination_msg it delivers
//   - a supervisor that restarts what it watches
//   - unwatch()
//
// ASSUMES
//   Tutorials 1-8.
//
// THE IDEA
//   Tutorial 8 left references to a stopped actor stale, with nobody told.
//   A watcher is sent an ordinary message when the watched actor terminates,
//   so it can drop the reference, fail over, or restart it.

#include "tutorials.h"
#include "cas/cas.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace {

// Nothing special: it neither knows nor cares that it is watched.
class fragile_worker : public cas::actor {
public:
    struct do_work : cas::message_base {};

protected:
    void on_start() override {
        set_name("fragile_worker");
        handler<do_work>([](const do_work&) { std::cout << "  fragile_worker did some work\n"; });
    }
};

class supervisor : public cas::actor {
public:
    static inline std::atomic<int> terminations{0};     // observed by the tutorial
    static inline std::atomic<int> restarts{0};

    static std::string last_terminated() {
        std::lock_guard<std::mutex> lock(s_mutex);
        return s_last_terminated;
    }

protected:
    void on_start() override {
        set_name("supervisor");
        // termination_msg comes from the framework but registers like any
        // other message.
        handler<cas::termination_msg>(&supervisor::on_termination);
    }

private:
    void on_termination(const cas::termination_msg& msg) {
        std::cout << "  supervisor: '" << msg.actor_name << "' terminated\n";
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_last_terminated = msg.actor_name;
        }
        terminations.fetch_add(1);

        // create() is fine inside a handler; the new actor starts at once.
        // Restart only once - an unbounded restart turns a permanently
        // failing actor into an endless loop.
        if (!m_restarted) {
            m_restarted = true;
            cas::system::create<fragile_worker>();
            std::cout << "  supervisor: restarted it\n";
            restarts.fetch_add(1);                       // after create() returned
        }
    }

    bool m_restarted = false;

    static inline std::mutex s_mutex;
    static inline std::string s_last_terminated;
};

} // namespace

namespace tut {

bool run_watch_pattern() {
    bool ok = true;

    section("Watching");
    auto worker_ref = cas::system::create<fragile_worker>();
    auto supervisor_ref = cas::system::create<supervisor>();
    cas::system::start();
    // watch(watcher, watched): one-way; an actor may watch several others.
    cas::system::watch(supervisor_ref, worker_ref);
    worker_ref.tell(fragile_worker::do_work{});

    section("Stopping the watched actor");
    cas::system::stop_actor(worker_ref);
    ok &= check(wait_until([] { return supervisor::terminations.load() == 1; }),
                "supervisor was notified of the termination");
    ok &= check(supervisor::last_terminated() == "fragile_worker",
                "termination_msg named the actor that stopped");
    ok &= check(wait_until([] { return supervisor::restarts.load() == 1; }),
                "supervisor restarted the worker");

    // The restarted actor is a new one; the old reference stays stale.
    auto restarted = cas::actor_registry::get("fragile_worker");
    ok &= check(restarted.is_valid() && restarted.is_running(),
                "the restarted worker is registered and running");
    ok &= check(!worker_ref.is_running(), "the original reference is still stale after the restart");

    section("Unwatching, then stopping again");
    // unwatch() spares the watcher a notification it would only ignore.
    cas::system::unwatch(supervisor_ref, restarted);
    const int before = supervisor::terminations.load();
    cas::system::stop_actor(restarted);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    ok &= check(supervisor::terminations.load() == before, "no notification arrived after unwatch()");

    section("Shutting down");
    cas::system::shutdown();
    cas::system::wait_for_shutdown();
    return ok;
}

} // namespace tut
