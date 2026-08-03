// Tutorial 14: Inline actors
//
// TEACHES
//   - inline_actor, which handles messages in the SENDER's thread
//   - what that buys and what it costs
//   - inline_actor<true> vs inline_actor<false>
//   - why an inline actor is not really an actor, and when that is fine
//
// ASSUMES
//   Tutorials 1-13.
//
// THE IDEA
//   Every actor so far has had a mailbox: tell() queues, and someone else's
//   thread runs the handler later. An inline_actor has no mailbox. tell()
//   runs the handler immediately, on the calling thread, before it returns.
//
//   That makes it the cheapest possible option and the least actor-like. It
//   is really a synchronous object wearing the actor interface, which is
//   useful when you want the interface without the queueing.

#include "tutorials.h"
#include "cas/cas.h"

#include <atomic>
#include <iostream>
#include <thread>
#include <vector>

namespace {

struct record : cas::message_base {
    int value = 0;
};

std::atomic<int> g_records{0};

// Which thread ran the handler? For an inline actor it is the sender's.
std::thread::id g_handler_thread;

// --- 1. A thread-safe inline actor -------------------------------------------
//
// inline_actor<true> takes a mutex around dispatch, so several threads may
// send to it concurrently. Use this unless you are certain there is only one
// sender.

class counter : public cas::inline_actor<true> {
protected:
    void on_start() override {
        set_name("counter");
        handler<record>(&counter::on_record);
    }

    void on_record(const record& msg) {
        (void)msg;
        g_handler_thread = std::this_thread::get_id();
        g_records.fetch_add(1);
    }
};

} // namespace

namespace tut {

bool run_inline_actor() {
    bool ok = true;

    section("Starting an inline actor");
    auto counter_ref = cas::system::create<counter>();
    cas::system::start();

    // --- 2. Dispatch is synchronous ------------------------------------------
    //
    // With an ordinary actor, the count would still be 0 here and we would
    // have to wait for it. With an inline actor the handler has already run
    // by the time tell() returns - no waiting, and no wait_until().

    section("tell() runs the handler before it returns");
    record r;
    r.value = 1;
    counter_ref.tell(r);

    ok &= check(g_records.load() == 1,
                "the handler had already run when tell() returned");

    // --- 3. And it runs on the calling thread --------------------------------
    //
    // This is the part to be careful about. The handler executes on whatever
    // thread called tell(), so:
    //
    //   - a slow handler blocks the sender
    //   - the actor's state is only protected by that mutex, not by the
    //     one-thread-per-actor rule that ordinary actors rely on
    //   - there is no mailbox, so nothing is queued or drained

    section("The handler runs on the sender's thread");
    ok &= check(g_handler_thread == std::this_thread::get_id(),
                "the handler ran on the calling thread, not a worker");

    // --- 4. Several threads sending at once ----------------------------------
    //
    // inline_actor<true> serialises concurrent senders with its mutex, so the
    // count is exact. Note what that implies: senders now contend for a lock,
    // where an ordinary actor's queue would have absorbed them.

    section("Concurrent senders");
    g_records.store(0);

    constexpr int k_threads = 4;
    constexpr int k_each = 250;

    std::vector<std::thread> senders;
    for (int t = 0; t < k_threads; ++t) {
        senders.emplace_back([counter_ref]() {
            for (int i = 0; i < k_each; ++i) {
                record msg;
                msg.value = i;
                counter_ref.tell(msg);
            }
        });
    }
    for (auto& s : senders) {
        s.join();
    }

    ok &= check(g_records.load() == k_threads * k_each,
                "all 1000 sends from 4 threads were counted exactly");

    // --- 5. Choosing ---------------------------------------------------------
    //
    // inline_actor<false> drops the mutex for the last bit of speed, and is
    // only safe with a single sender - a violation is a data race, not an
    // error you will be told about. This tutorial does not demonstrate it for
    // that reason.
    //
    // Reach for an inline actor when a component should look like an actor to
    // its callers but has no reason to own a thread: a validator, a stateless
    // transformer, a counter. If it needs to isolate state under concurrency,
    // to drain on shutdown, or to keep a slow handler off the caller's thread,
    // use an ordinary actor.

    section("Shutting down");
    cas::system::shutdown();
    cas::system::wait_for_shutdown();

    return ok;
}

} // namespace tut
