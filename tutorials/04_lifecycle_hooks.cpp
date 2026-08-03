// Tutorial 4: Lifecycle hooks
//
// TEACHES
//   - on_start(), on_shutdown() and on_stop(), and what each is for
//   - the order they run in
//   - why on_shutdown() may send messages and on_stop() may not
//   - message draining: queued work still runs during shutdown
//
// ASSUMES
//   Tutorials 1-3.
//
// THE IDEA
//   An actor has three moments where it can act outside of handling a
//   message: when it starts, when it is told to shut down, and when it has
//   finally stopped. Putting work in the wrong one is a common source of
//   messages that vanish or resources that leak.

#include "tutorials.h"
#include "cas/cas.h"

#include <atomic>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

namespace {

struct work : cas::message_base {
    int id = 0;
};

struct farewell : cas::message_base {};

// Records the order hooks and handlers ran in, so the tutorial can assert on
// it after the system has stopped.
std::mutex g_log_mutex;
std::vector<std::string> g_log;

void note(const std::string& what) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    g_log.push_back(what);
    std::cout << "  " << what << "\n";
}

std::atomic<int> g_work_done{0};
std::atomic<int> g_farewells_received{0};

// A second actor, only here to prove on_shutdown() can still send messages.
class undertaker : public cas::actor {
protected:
    void on_start() override {
        set_name("undertaker");
        handler<farewell>(&undertaker::on_farewell);
    }

    void on_farewell(const farewell&) {
        note("undertaker received a farewell message");
        g_farewells_received.fetch_add(1);
    }
};

// --- The three hooks ---------------------------------------------------------

class worker : public cas::actor {
public:
    explicit worker(cas::actor_ref undertaker_ref)
        : m_undertaker(undertaker_ref) {}

protected:
    // 1. on_start() - runs once, before any message is delivered.
    //
    // Register handlers here. Also the place to acquire resources the actor
    // needs. It runs on the actor's own thread, so anything it touches is
    // already protected by the one-thread-per-actor rule.
    void on_start() override {
        set_name("worker");
        handler<work>(&worker::on_work);
        note("on_start");
    }

    // 2. on_shutdown() - runs when shutdown begins.
    //
    // Sending is *allowed* here, and this is the place to flush state, close
    // connections, or notify a collaborator. But note the limit carefully:
    //
    //   sending is permitted; DELIVERY IS NOT GUARANTEED.
    //
    // A system-wide shutdown queues a shutdown message to every actor at
    // once, so the intended recipient may already be stopping and refusing
    // new messages. Whether this farewell arrives is a race, which is why
    // the check below accepts either outcome.
    //
    // The rule: use on_shutdown() to put your own house in order. If another
    // actor MUST hear from you, send it while the system is running normally,
    // not on the way out.
    void on_shutdown() override {
        note("on_shutdown");
        m_undertaker.tell(farewell{});
    }

    // 3. on_stop() - runs after the actor has fully stopped.
    //
    // No messages can be sent from here; the actor is done and the machinery
    // that would deliver them is going away. Use it to release resources and
    // publish final state, as this tutorial does.
    //
    // Sending from here is not an error that will be reported - the message
    // simply goes nowhere, which is why the distinction matters.
    void on_stop() override {
        note("on_stop");
    }

    void on_work(const work& msg) {
        note("handled work " + std::to_string(msg.id));
        g_work_done.fetch_add(1);
    }

private:
    cas::actor_ref m_undertaker;
};

} // namespace

namespace tut {

bool run_lifecycle_hooks() {
    bool ok = true;

    section("Starting");
    auto undertaker_ref = cas::system::create<undertaker>();
    auto worker_ref = cas::system::create<worker>(undertaker_ref);
    cas::system::start();

    // --- Draining ------------------------------------------------------------
    //
    // Queue several messages and shut down immediately, without waiting. The
    // messages are already in the mailbox, so shutdown drains them rather than
    // discarding them: shutdown is graceful by default.

    section("Queueing work, then shutting down at once");
    for (int i = 1; i <= 3; ++i) {
        work w;
        w.id = i;
        worker_ref.tell(w);
    }

    cas::system::shutdown();
    cas::system::wait_for_shutdown();

    // --- What we expect ------------------------------------------------------

    section("Checking what happened");

    ok &= check(g_work_done.load() == 3,
                "all 3 queued messages were drained, not dropped");

    // Deliberately NOT asserting that the farewell arrived. During a
    // system-wide shutdown every actor is told to stop at the same time, so
    // the undertaker may already have stopped accepting messages. Both
    // outcomes are correct; asserting either one would make this tutorial
    // flaky and would teach a guarantee the framework does not give.
    std::cout << "  (farewell sent from on_shutdown() "
              << (g_farewells_received.load() == 1 ? "arrived this run"
                                                   : "did not arrive this run")
              << " - both are valid)\n";

    // Hook order: on_start first, on_stop last, and every unit of work
    // between on_start and on_stop.
    std::lock_guard<std::mutex> lock(g_log_mutex);
    const bool starts_first = !g_log.empty() && g_log.front() == "on_start";
    ok &= check(starts_first, "on_start() ran first");

    auto index_of = [](const std::string& what) -> int {
        for (size_t i = 0; i < g_log.size(); ++i) {
            if (g_log[i] == what) {
                return static_cast<int>(i);
            }
        }
        return -1;
    };

    const int shutdown_at = index_of("on_shutdown");
    const int stop_at = index_of("on_stop");
    ok &= check(shutdown_at >= 0 && stop_at >= 0 && shutdown_at < stop_at,
                "on_shutdown() ran before on_stop()");

    const int last_work_at = index_of("handled work 3");
    ok &= check(last_work_at >= 0 && last_work_at < stop_at,
                "queued work was handled before on_stop()");

    return ok;
}

} // namespace tut
