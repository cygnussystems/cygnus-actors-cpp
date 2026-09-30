// Tutorial 8: Stopping actors at runtime
//
// TEACHES
//   - system::stop_actor() while the system keeps running
//   - stop_mode::drain vs stop_mode::discard
//   - what an actor_ref to a stopped actor still does
//   - when a stopped actor is actually destroyed, and actor_ref::reset()
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
#include <iostream>
#include <string>

namespace {

// The messages.
namespace msg {

struct job : cas::message_base {
    int id = 0;
};

} // namespace msg

// The actors.
namespace actor {

class worker : public cas::actor {
public:
    static inline std::atomic<int> s_job_count{0};       // observed by the tutorial
    static inline std::atomic<int> s_destroyed_count{0};

    explicit worker(std::string name) : m_name(name) {}
    ~worker() override { s_destroyed_count.fetch_add(1); }

protected:
    void on_start() override {
        set_name(m_name);
        handler<msg::job>(&worker::on_job);
    }

    void on_stop() override { std::cout << "  " << m_name << " stopped\n"; }

private:
    void on_job(const msg::job& _msg) {
        std::cout << "  " << m_name << " handled job " << _msg.id << "\n";
        s_job_count.fetch_add(1);
    }

    std::string m_name;
};

} // namespace actor

void send_job(const cas::actor_ref& to, int id) {
    msg::job job;
    job.id = id;
    to.tell(job);
}

} // namespace

namespace tut {

bool run_dynamic_removal() {
    bool ok = true;

    heading("Starting a worker");
    //---------------------------
    auto worker = cas::system::create<actor::worker>("worker_a");
    cas::system::start();
    send_job(worker, 1);
    ok &= check(wait_until(actor::worker::s_job_count, 1), "worker handled a job normally");

    heading("Stopping the worker (default: drain)");
    //----------------------------------------------
    // stop_actor() stops one actor; the system keeps running. By default it
    // drains - queued messages are handled first - and blocks until done.
    ok &= check(cas::system::stop_actor(worker), "stop_actor() reported success");
    ok &= check(cas::system::is_running(), "system still running after stopping one actor");

    heading("The reference is now stale");
    //------------------------------------
    // An actor_ref holds on to the actor object, so it never dangles: it
    // still refers to the (stopped) actor and is safe to use. Messages
    // through it are dropped and counted as dead letters (tutorial 10).
    ok &= check(worker.is_valid() && !worker.is_running(), "still a valid handle, but not running");
    const size_t dropped_before = cas::system::get_dead_letter_stats().dropped_tell;
    send_job(worker, 99);
    ok &= check(cas::system::get_dead_letter_stats().dropped_tell == dropped_before + 1,
                "a message to the stopped actor became a dead letter");

    heading("When the actor goes away");
    //----------------------------------
    // There is no separate "delete": stopping removes the actor from the
    // system, and the object is destroyed when the last actor_ref to it is
    // released. Our handle is the last one, so it is still alive.
    ok &= check(actor::worker::s_destroyed_count.load() == 0, "alive while a handle refers to it");
    worker.reset();  // release the handle (like std::shared_ptr::reset())
    ok &= check(wait_until(actor::worker::s_destroyed_count, 1), "destroyed once the last handle was released");
    // Handles kept in other actors count too: two actors holding refs to each
    // other keep each other alive, so clear such members in on_stop().

    heading("Stopping with stop_mode::discard");
    //------------------------------------------
    // discard drops queued messages instead of handling them: for work that
    // is worthless once the actor goes away.
    auto second = cas::system::create<actor::worker>("worker_b");
    const int before_b = actor::worker::s_job_count.load();
    for (int i = 1; i <= 5; ++i) {
        send_job(second, i);
    }
    cas::stop_config discard;
    discard.mode = cas::stop_mode::discard;
    cas::system::stop_actor(second, discard);
    // Some may have run before the stop landed; discard only skips the rest.
    std::cout << "  worker_b handled " << actor::worker::s_job_count.load() - before_b
              << " of 5 queued jobs before being discarded\n";
    ok &= check(!second.is_running(), "discarded actor is no longer running");

    heading("Creating a replacement under the same name");
    //----------------------------------------------------
    // A stopped actor's name is free again - the basis of restarting a worker.
    auto replacement = cas::system::create<actor::worker>("worker_a");
    ok &= check(replacement.is_running(), "replacement actor is running");
    const int before_replacement = actor::worker::s_job_count.load();
    send_job(replacement, 2);
    ok &= check(wait_until(actor::worker::s_job_count, before_replacement + 1),
                "the replacement handled a job");

    heading("Shutting down");
    //-----------------------
    cas::system::shutdown();
    cas::system::wait_for_shutdown();
    return ok;
}

} // namespace tut
