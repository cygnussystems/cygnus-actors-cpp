// Tutorial 4: Lifecycle hooks
//
// TEACHES
//   - on_start(), on_shutdown() and on_stop(), what each is for and their order
//   - why on_shutdown() may send messages and on_stop() may not
//   - message draining: queued work still runs during shutdown
//
// ASSUMES
//   Tutorials 1-3.
//
// THE IDEA
//   Besides handling messages, an actor can act at three moments: when it
//   starts, when it is told to shut down, and when it has finally stopped.
//   Work in the wrong hook means messages that vanish or resources that leak.

#include "tutorials.h"
#include "cas/cas.h"

#include <algorithm>
#include <atomic>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

namespace {

// Only here to receive a message sent from the worker's on_shutdown().
class undertaker : public cas::actor {
public:
    struct farewell : cas::message_base {};

    static inline std::atomic<int> farewells{0};  // observed by the tutorial

protected:
    void on_start() override {
        set_name("undertaker");
        handler<farewell>(&undertaker::on_farewell);
    }

private:
    void on_farewell(const farewell&) {
        std::cout << "  undertaker received a farewell\n";
        farewells.fetch_add(1);
    }
};

class worker : public cas::actor {
public:
    struct work : cas::message_base {
        int id = 0;
    };

    explicit worker(cas::actor_ref undertaker) : m_undertaker(std::move(undertaker)) {}

    // Observed by the tutorial: the order hooks and handlers ran in.
    static std::vector<std::string> history() {
        std::lock_guard<std::mutex> lock(s_mutex);
        return s_history;
    }

protected:
    // 1. Once, on the actor's thread, before any message: register handlers,
    //    acquire resources.
    void on_start() override {
        set_name("worker");
        handler<work>(&worker::on_work);
        note("on_start");
    }

    // 2. When shutdown begins: put your own house in order. Sending is
    //    allowed but delivery is NOT guaranteed - a system-wide shutdown stops
    //    every actor at once, so the recipient may already refuse messages.
    //    If another actor must hear from you, tell it while the system runs.
    void on_shutdown() override {
        note("on_shutdown");
        m_undertaker.tell(undertaker::farewell{});
    }

    // 3. After the actor has fully stopped: release resources, publish final
    //    state. A message sent from here silently goes nowhere.
    void on_stop() override { note("on_stop"); }

private:
    void on_work(const work& msg) { note("handled work " + std::to_string(msg.id)); }

    static void note(const std::string& what) {
        std::cout << "  " << what << "\n";
        std::lock_guard<std::mutex> lock(s_mutex);
        s_history.push_back(what);
    }

    static inline std::mutex s_mutex;
    static inline std::vector<std::string> s_history;

    cas::actor_ref m_undertaker;
};

} // namespace

namespace tut {

bool run_lifecycle_hooks() {
    bool ok = true;

    heading("Starting");
    auto undertaker_ref = cas::system::create<undertaker>();
    auto worker_ref = cas::system::create<worker>(undertaker_ref);
    cas::system::start();

    heading("Queueing work, then shutting down at once");
    // The messages are already in the mailbox, so shutdown drains them rather
    // than discarding them: shutdown is graceful by default.
    for (int i = 1; i <= 3; ++i) {
        worker::work w;
        w.id = i;
        worker_ref.tell(w);
    }
    cas::system::shutdown();
    cas::system::wait_for_shutdown();

    heading("Checking what happened");
    const auto log = worker::history();
    auto at = [&](const std::string& what) {
        return static_cast<int>(std::find(log.begin(), log.end(), what) - log.begin());
    };
    const int end = static_cast<int>(log.size());

    ok &= check(std::count_if(log.begin(), log.end(),
                              [](const std::string& s) { return s.rfind("handled", 0) == 0; }) == 3,
                "all 3 queued messages were drained, not dropped");
    ok &= check(!log.empty() && log.front() == "on_start", "on_start() ran first");
    ok &= check(at("on_shutdown") < at("on_stop") && at("on_stop") < end,
                "on_shutdown() ran before on_stop()");
    ok &= check(at("handled work 3") < at("on_stop"), "queued work was handled before on_stop()");

    // Deliberately not asserted: whether the farewell arrived is a race, and
    // both outcomes are correct.
    std::cout << "  (farewell from on_shutdown() "
              << (undertaker::farewells.load() == 1 ? "arrived" : "did not arrive")
              << " this run - both are valid)\n";

    return ok;
}

} // namespace tut
