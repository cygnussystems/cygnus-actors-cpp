// Tutorial 8: Stopping actors at runtime
//
// TEACHES
//   - system::stop_actor() while the system keeps running
//   - stop_mode::drain vs stop_mode::discard
//   - is_running() on an actor_ref, and what a stale ref does
//   - creating a replacement actor after stopping one
//
// ASSUMES
//   Tutorials 1-7.
//
// THE IDEA
//   Shutting the whole system down is the easy case. Stopping one actor while
//   everything else keeps working is the interesting one: you have to decide
//   what happens to the messages already in its mailbox, and every reference
//   held elsewhere becomes stale.

#include "tutorials.h"
#include "cas/cas.h"

#include <atomic>
#include <iostream>

namespace {

struct job : cas::message_base {
    int id = 0;
};

std::atomic<int> g_jobs_done{0};
std::atomic<int> g_replacement_jobs{0};

class worker : public cas::actor {
public:
    explicit worker(std::string name, std::atomic<int>* counter)
        : m_name(std::move(name)), m_counter(counter) {}

protected:
    void on_start() override {
        set_name(m_name);
        handler<job>(&worker::on_job);
    }

    void on_job(const job& msg) {
        std::cout << "  " << m_name << " handled job " << msg.id << "\n";
        m_counter->fetch_add(1);
    }

    void on_stop() override {
        std::cout << "  " << m_name << " stopped\n";
    }

private:
    std::string m_name;
    std::atomic<int>* m_counter;
};

} // namespace

namespace tut {

bool run_dynamic_removal() {
    bool ok = true;

    section("Starting a worker");
    auto worker_ref = cas::system::create<worker>("worker_a", &g_jobs_done);
    cas::system::start();

    job j;
    j.id = 1;
    worker_ref.tell(j);
    ok &= check(wait_until([] { return g_jobs_done.load() == 1; }),
                "worker handled a job normally");

    // --- 1. Stopping one actor -----------------------------------------------
    //
    // stop_actor() stops that actor and leaves the rest of the system
    // running. By default it drains: messages already queued are processed
    // before the actor stops, and the call blocks until that is done.

    section("Stopping the worker (default: drain)");
    ok &= check(cas::system::is_running(),
                "system still running before stopping the actor");

    const bool stopped = cas::system::stop_actor(worker_ref);
    ok &= check(stopped, "stop_actor() reported success");
    ok &= check(cas::system::is_running(),
                "system still running after stopping one actor");

    // --- 2. A stale reference ------------------------------------------------
    //
    // worker_ref still points at the stopped actor. It is not dangling - it is
    // safe to call - but the actor is no longer running, and messages sent
    // through it are dropped silently. Check before use if it matters; the
    // dead letter tutorial shows how to observe the drops.

    section("The reference is now stale");
    ok &= check(!worker_ref.is_running(),
                "is_running() is false for the stopped actor");

    const int before = g_jobs_done.load();
    job ignored;
    ignored.id = 99;
    worker_ref.tell(ignored);  // goes nowhere, no error
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ok &= check(g_jobs_done.load() == before,
                "a message to a stopped actor was silently dropped");

    // --- 3. Draining vs discarding -------------------------------------------
    //
    // stop_config chooses what happens to queued messages. drain (the default)
    // processes them first; discard drops them immediately. Use discard when
    // the pending work is worthless once the actor is going away - a shutdown
    // where the results would be thrown out anyway.

    section("Stopping with stop_mode::discard");
    auto second = cas::system::create<worker>("worker_b", &g_replacement_jobs);

    for (int i = 1; i <= 5; ++i) {
        job queued;
        queued.id = i;
        second.tell(queued);
    }

    cas::stop_config discard_config;
    discard_config.mode = cas::stop_mode::discard;
    cas::system::stop_actor(second, discard_config);

    // Some of the five may already have been handled before the stop landed;
    // the guarantee is only that discarding does not wait for the rest.
    std::cout << "  worker_b handled " << g_replacement_jobs.load()
              << " of 5 queued jobs before being discarded\n";
    ok &= check(!second.is_running(), "discarded actor is no longer running");

    // --- 4. Replacing an actor -----------------------------------------------
    //
    // A name is free again once its actor stops, so a replacement can take
    // over the same identity - the basis of restarting a failed worker.

    section("Creating a replacement under the same name");
    auto replacement = cas::system::create<worker>("worker_a", &g_jobs_done);
    ok &= check(replacement.is_running(), "replacement actor is running");

    const int before_replacement = g_jobs_done.load();
    job for_replacement;
    for_replacement.id = 2;
    replacement.tell(for_replacement);
    ok &= check(wait_until([&] { return g_jobs_done.load() > before_replacement; }),
                "the replacement handled a job");

    section("Shutting down");
    cas::system::shutdown();
    cas::system::wait_for_shutdown();

    return ok;
}

} // namespace tut
