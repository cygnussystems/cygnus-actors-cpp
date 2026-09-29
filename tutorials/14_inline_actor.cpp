// Tutorial 14: Inline actors
//
// TEACHES
//   - inline_actor, which handles messages on the SENDER's thread
//   - what that buys and what it costs
//   - inline_actor<true> vs inline_actor<false>
//
// ASSUMES
//   Tutorials 1-13.
//
// THE IDEA
//   An inline_actor has no mailbox: tell() runs the handler immediately, on
//   the calling thread, before it returns. The cheapest option and the least
//   actor-like - a synchronous object wearing the actor interface.
//
//   Reach for one when a component should look like an actor but has no reason
//   to own a thread: a validator, a transformer, a counter. If it must isolate
//   state from slow handlers, drain on shutdown, or keep work off the caller's
//   thread, use an ordinary actor.

#include "tutorials.h"
#include "cas/cas.h"

#include <atomic>
#include <thread>
#include <vector>

namespace {

// The messages. Sent to an inline actor exactly as to any other.
namespace msg {

struct record : cas::message_base {
    int value = 0;
};

} // namespace msg

// The actors.
namespace actor {

// inline_actor<true> takes a mutex around dispatch, so several threads may
// send at once. inline_actor<false> drops the mutex and is safe only with a
// single sender - a violation is a silent data race, so it is not shown here.
class counter : public cas::inline_actor<true> {
public:
    // Observed by the tutorial: how many records, and which thread ran the
    // last handler.
    static inline std::atomic<int> s_record_count{0};
    static inline std::atomic<std::thread::id> s_handler_thread{};

    // Sends `count` records to `to` - run on several threads at once below
    static void send_records(cas::actor_ref to, int count) {
        for (int i = 0; i < count; ++i) {
            msg::record entry;
            entry.value = i;
            to.tell(entry);
        }
    }

protected:
    void on_start() override {
        set_name("counter");
        handler<msg::record>(&counter::on_record);
    }

private:
    void on_record(const msg::record&) {
        s_handler_thread.store(std::this_thread::get_id());
        s_record_count.fetch_add(1);
    }
};

} // namespace actor

} // namespace

namespace tut {

bool run_inline_actor() {
    bool ok = true;

    heading("Starting an inline actor");
    //----------------------------------
    auto counter_ref = cas::system::create<actor::counter>();
    cas::system::start();

    heading("tell() runs the handler before it returns");
    //---------------------------------------------------
    // With an ordinary actor we would need wait_until() here. Not with this one.
    counter_ref.tell(msg::record{});
    ok &= check(actor::counter::s_record_count.load() == 1, "the handler had already run when tell() returned");

    heading("The handler runs on the sender's thread");
    //-------------------------------------------------
    // So a slow handler blocks the sender, and the actor's state is protected
    // only by the mutex, not by the one-thread-per-actor rule.
    ok &= check(actor::counter::s_handler_thread.load() == std::this_thread::get_id(),
                "the handler ran on the calling thread, not a worker");

    heading("Concurrent senders");
    //----------------------------
    // The mutex serialises senders, so the count is exact - but they now
    // contend for a lock where an ordinary actor's mailbox would absorb them.
    actor::counter::s_record_count.store(0);
    constexpr int thread_count = 4;
    constexpr int per_thread = 250;
    std::vector<std::thread> senders;
    for (int t = 0; t < thread_count; ++t) {
        senders.emplace_back(&actor::counter::send_records, counter_ref, per_thread);
    }
    for (auto& s : senders) {
        s.join();
    }
    ok &= check(actor::counter::s_record_count.load() == thread_count * per_thread,
                "all 1000 sends from 4 threads were counted exactly");

    heading("Shut down");
    //-------------------
    cas::system::shutdown();
    cas::system::wait_for_shutdown();
    return ok;
}

} // namespace tut
