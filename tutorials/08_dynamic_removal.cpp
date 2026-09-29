// Tutorial 8: Stopping actors at runtime
//
// TEACHES
//   - system::stop_actor() while the system keeps running
//   - stop_mode::drain vs stop_mode::discard
//   - is_running() on an actor_ref, and what a stale ref does
//   - creating a replacement actor under the same name
//
// ASSUMES
//   Tutorials 1-7.
//
// THE IDEA
//   Stopping one actor while everything else keeps working means deciding
//   what happens to the messages already in its mailbox - and every reference
//   held elsewhere becomes stale.

#include "tutorials.h"
#include "cas/cas.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace {

class worker : public cas::actor {
public:
    struct job : cas::message_base {
        int id = 0;
    };

    static inline std::atomic<int> jobs_done{0};         // observed by the tutorial

    explicit worker(std::string name) : m_name(std::move(name)) {}

protected:
    void on_start() override {
        set_name(m_name);
        handler<job>(&worker::on_job);
    }

    void on_stop() override { std::cout << "  " << m_name << " stopped\n"; }

private:
    void on_job(const job& msg) {
        std::cout << "  " << m_name << " handled job " << msg.id << "\n";
        jobs_done.fetch_add(1);
    }

    std::string m_name;
};

void send_job(const cas::actor_ref& to, int id) {
    worker::job msg;
    msg.id = id;
    to.tell(msg);
}

} // namespace

namespace tut {

bool run_dynamic_removal() {
    bool ok = true;

    heading("Starting a worker");
    auto worker_ref = cas::system::create<worker>("worker_a");
    cas::system::start();
    send_job(worker_ref, 1);
    ok &= check(wait_until([] { return worker::jobs_done.load() == 1; }), "worker handled a job normally");

    heading("Stopping the worker (default: drain)");
    // stop_actor() stops one actor; the system keeps running. By default it
    // drains - queued messages are handled first - and blocks until done.
    ok &= check(cas::system::stop_actor(worker_ref), "stop_actor() reported success");
    ok &= check(cas::system::is_running(), "system still running after stopping one actor");

    heading("The reference is now stale");
    // Not dangling - safe to use - but messages through it are silently
    // dropped (tutorial 10 shows how to observe that).
    ok &= check(!worker_ref.is_running(), "is_running() is false for the stopped actor");
    const int before = worker::jobs_done.load();
    send_job(worker_ref, 99);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ok &= check(worker::jobs_done.load() == before, "a message to a stopped actor was silently dropped");

    heading("Stopping with stop_mode::discard");
    // discard drops queued messages instead of handling them: for work that
    // is worthless once the actor goes away.
    auto second = cas::system::create<worker>("worker_b");
    const int before_b = worker::jobs_done.load();
    for (int i = 1; i <= 5; ++i) {
        send_job(second, i);
    }
    cas::stop_config discard;
    discard.mode = cas::stop_mode::discard;
    cas::system::stop_actor(second, discard);
    // Some may have run before the stop landed; discard only skips the rest.
    std::cout << "  worker_b handled " << worker::jobs_done.load() - before_b
              << " of 5 queued jobs before being discarded\n";
    ok &= check(!second.is_running(), "discarded actor is no longer running");

    heading("Creating a replacement under the same name");
    // A stopped actor's name is free again - the basis of restarting a worker.
    auto replacement = cas::system::create<worker>("worker_a");
    ok &= check(replacement.is_running(), "replacement actor is running");
    const int before_replacement = worker::jobs_done.load();
    send_job(replacement, 2);
    ok &= check(wait_until([&] { return worker::jobs_done.load() > before_replacement; }),
                "the replacement handled a job");

    heading("Shutting down");
    cas::system::shutdown();
    cas::system::wait_for_shutdown();
    return ok;
}

} // namespace tut
