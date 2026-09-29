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

// The messages.
namespace msg {

struct work : cas::message_base {
    int id = 0;
};

// From the worker's on_shutdown() to the undertaker.
struct farewell : cas::message_base {};

} // namespace msg

// The actors.
namespace actor {

// Only here to receive a message sent from the worker's on_shutdown().
class undertaker : public cas::actor {
public:
    static inline std::atomic<int> s_farewell_count{0};  // observed by the tutorial

protected:
    void on_start() override {
        set_name("undertaker");
        handler<msg::farewell>(&undertaker::on_farewell);
    }

private:
    void on_farewell(const msg::farewell&) {
        std::cout << "  undertaker received a farewell\n";
        s_farewell_count.fetch_add(1);
    }
};

class worker : public cas::actor {
public:
    explicit worker(cas::actor_ref undertaker)
        : m_undertaker(undertaker) {}

    // Observed by the tutorial: the order hooks and handlers ran in.
    static std::vector<std::string> history() {
        std::lock_guard<std::mutex> lock(s_mutex);
        return s_history;
    }

    // Where `what` appears in the history (history().size() if it never ran)
    static int position_of(const std::string& what) {
        std::vector<std::string> log = history();
        for (size_t i = 0; i < log.size(); ++i) {
            if (log[i] == what) {
                return static_cast<int>(i);
            }
        }
        return static_cast<int>(log.size());
    }

    // How many work messages were handled
    static int work_handled() {
        int count = 0;
        for (const std::string& entry : history()) {
            if (entry.rfind("handled work", 0) == 0) {
                ++count;
            }
        }
        return count;
    }

protected:
    // 1. Once, on the actor's thread, before any message: register handlers,
    //    acquire resources.
    void on_start() override {
        set_name("worker");
        handler<msg::work>(&worker::on_work);
        note("on_start");
    }

    // 2. When shutdown begins: put your own house in order. Sending is
    //    allowed but delivery is NOT guaranteed - a system-wide shutdown stops
    //    every actor at once, so the recipient may already refuse messages.
    //    If another actor must hear from you, tell it while the system runs.
    void on_shutdown() override {
        note("on_shutdown");
        m_undertaker.tell(msg::farewell{});
    }

    // 3. After the actor has fully stopped: release resources, publish final
    //    state. A message sent from here silently goes nowhere.
    void on_stop() override { note("on_stop"); }

private:
    void on_work(const msg::work& _msg) { note("handled work " + std::to_string(_msg.id)); }

    static void note(const std::string& what) {
        std::cout << "  " << what << "\n";
        std::lock_guard<std::mutex> lock(s_mutex);
        s_history.push_back(what);
    }

    static inline std::mutex s_mutex;
    static inline std::vector<std::string> s_history;

    cas::actor_ref m_undertaker;
};

} // namespace actor

} // namespace

namespace tut {

bool run_lifecycle_hooks() {
    bool ok = true;

    heading("Starting");
    //------------------
    auto undertaker = cas::system::create<actor::undertaker>();
    auto worker = cas::system::create<actor::worker>(undertaker);
    cas::system::start();

    heading("Queueing work, then shutting down at once");
    //---------------------------------------------------
    // The messages are already in the mailbox, so shutdown drains them rather
    // than discarding them: shutdown is graceful by default.
    for (int i = 1; i <= 3; ++i) {
        msg::work job;
        job.id = i;
        worker.tell(job);
    }
    cas::system::shutdown();
    cas::system::wait_for_shutdown();

    heading("Checking what happened");
    //--------------------------------
    const int end = static_cast<int>(actor::worker::history().size());

    ok &= check(actor::worker::work_handled() == 3, "all 3 queued messages were drained, not dropped");
    ok &= check(actor::worker::position_of("on_start") == 0, "on_start() ran first");
    ok &= check(actor::worker::position_of("on_shutdown") < actor::worker::position_of("on_stop") &&
                    actor::worker::position_of("on_stop") < end,
                "on_shutdown() ran before on_stop()");
    ok &= check(actor::worker::position_of("handled work 3") < actor::worker::position_of("on_stop"),
                "queued work was handled before on_stop()");

    // Deliberately not asserted: whether the farewell arrived is a race, and
    // both outcomes are correct.
    std::cout << "  (farewell from on_shutdown() "
              << (actor::undertaker::s_farewell_count.load() == 1 ? "arrived" : "did not arrive")
              << " this run - both are valid)\n";

    return ok;
}

} // namespace tut
