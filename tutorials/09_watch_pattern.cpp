// Tutorial 9: Watching an actor for termination
//
// TEACHES
//   - system::watch() and the termination_msg it delivers
//   - reacting to a collaborator stopping
//   - unwatch()
//   - a supervisor that restarts what it watches
//
// ASSUMES
//   Tutorials 1-8.
//
// THE IDEA
//   Tutorial 8 stopped an actor and left every reference to it stale, with
//   nothing told about it. Watching closes that gap: the watcher is sent an
//   ordinary message when the watched actor terminates, so it can drop the
//   reference, fail over, or restart it.

#include "tutorials.h"
#include "cas/cas.h"

#include <atomic>
#include <iostream>
#include <string>

namespace {

struct do_work : cas::message_base {};

std::atomic<int> g_terminations_seen{0};
std::atomic<int> g_restarts{0};
std::string g_last_terminated_name;
std::mutex g_name_mutex;

// The actor being watched. Nothing about it is special - it does not know or
// care that it is watched.
class fragile_worker : public cas::actor {
protected:
    void on_start() override {
        set_name("fragile_worker");
        handler<do_work>(&fragile_worker::on_do_work);
    }

    void on_do_work(const do_work&) {
        std::cout << "  fragile_worker did some work\n";
    }
};

// --- The supervisor ----------------------------------------------------------
//
// Handles cas::termination_msg, a framework message delivered to watchers when
// a watched actor stops. Registering a handler for it is all that is needed;
// the watch itself is set up with system::watch().

class supervisor : public cas::actor {
protected:
    void on_start() override {
        set_name("supervisor");

        // termination_msg is defined by the framework, not by us, but it is an
        // ordinary message and registers like any other.
        handler<cas::termination_msg>(&supervisor::on_termination);
    }

    void on_termination(const cas::termination_msg& msg) {
        std::cout << "  supervisor: '" << msg.actor_name << "' terminated\n";
        g_terminations_seen.fetch_add(1);

        {
            std::lock_guard<std::mutex> lock(g_name_mutex);
            g_last_terminated_name = msg.actor_name;
        }

        // Restarting from here is safe: create() may be called from inside a
        // handler, and the new actor starts immediately.
        //
        // A real supervisor would bound this - restarting unconditionally
        // turns a permanently failing actor into an infinite restart loop.
        if (g_restarts.load() == 0) {
            g_restarts.fetch_add(1);
            cas::system::create<fragile_worker>();
            std::cout << "  supervisor: restarted it\n";
        }
    }
};

} // namespace

namespace tut {

bool run_watch_pattern() {
    bool ok = true;

    section("Starting a worker and its supervisor");
    auto worker_ref = cas::system::create<fragile_worker>();
    auto supervisor_ref = cas::system::create<supervisor>();
    cas::system::start();

    // --- 1. Establishing the watch -------------------------------------------
    //
    // watch(watcher, watched): the first argument is told when the second
    // one stops. The relationship is one-way, and an actor may watch several
    // others.

    section("Watching");
    cas::system::watch(supervisor_ref, worker_ref);

    worker_ref.tell(do_work{});
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    // --- 2. Termination notifies the watcher ---------------------------------

    section("Stopping the watched actor");
    cas::system::stop_actor(worker_ref);

    ok &= check(wait_until([] { return g_terminations_seen.load() == 1; }),
                "supervisor was notified of the termination");

    {
        std::lock_guard<std::mutex> lock(g_name_mutex);
        ok &= check(g_last_terminated_name == "fragile_worker",
                    "termination_msg named the actor that stopped");
    }

    ok &= check(g_restarts.load() == 1, "supervisor restarted the worker");

    // The restarted actor is a new one; the old reference stays stale.
    auto restarted = cas::actor_registry::get("fragile_worker");
    ok &= check(restarted.is_valid() && restarted.is_running(),
                "the restarted worker is registered and running");
    ok &= check(!worker_ref.is_running(),
                "the original reference is still stale after the restart");

    // --- 3. unwatch() --------------------------------------------------------
    //
    // Stops the notifications. Worth doing when the watcher no longer cares,
    // so it is not woken by a termination it will only ignore.

    section("Unwatching, then stopping again");
    cas::system::unwatch(supervisor_ref, restarted);

    const int before = g_terminations_seen.load();
    cas::system::stop_actor(restarted);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    ok &= check(g_terminations_seen.load() == before,
                "no notification arrived after unwatch()");

    section("Shutting down");
    cas::system::shutdown();
    cas::system::wait_for_shutdown();

    return ok;
}

} // namespace tut
