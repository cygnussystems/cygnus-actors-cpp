// Tutorial 14: Fast actors
//
// TEACHES
//   - fast_actor, which runs on its own dedicated thread
//   - the polling strategies and what each trades away
//   - when a dedicated thread is worth it, and when it is waste
//
// ASSUMES
//   Tutorials 1-13.
//
// THE IDEA
//   Ordinary actors share a pool of workers - thousands of actors, few
//   threads - so a message may wait for a worker. A fast_actor owns a thread
//   that polls its mailbox continuously, trading CPU for latency. That does
//   not scale: use it for the few actors on the critical path (a market data
//   feed, a control loop). More fast actors than cores runs slower, not faster.

#include "tutorials.h"
#include "cas/cas.h"

#include <atomic>
#include <chrono>
#include <iostream>

namespace {

// The messages. A fast actor receives the same messages as any other actor.
namespace msg {

struct market_tick : cas::message_base {
    double price = 0.0;
};

} // namespace msg

// The actors.
namespace actor {

// Derives from cas::fast_actor instead of cas::actor. Handlers, on_start()
// and messages are unchanged - only the execution model differs.
class price_engine : public cas::fast_actor {
public:
    static inline std::atomic<int> s_tick_count{0};      // observed by the tutorial

protected:
    void on_start() override {
        set_name("price_engine");
        handler<msg::market_tick>(&price_engine::on_tick);

        // What the thread does when the mailbox is empty:
        //   yield     - give up the CPU; ~1-10us latency, a good neighbour.
        //               The default, and right unless measurement says otherwise.
        //   hybrid    - spin briefly, then yield; sub-microsecond, moderate CPU.
        //   busy_wait - never yield; lowest latency, a core pinned at 100% for
        //               the actor's lifetime. Only with a core to spare.
        set_polling_strategy(cas::polling_strategy::yield);
    }

private:
    void on_tick(const msg::market_tick&) { s_tick_count.fetch_add(1); }
};

} // namespace actor

} // namespace

namespace tut {

bool run_fast_actor() {
    bool ok = true;

    heading("Starting a fast actor");
    //-------------------------------
    // From here on the actor's thread polls, even with nothing to do - that is
    // the price paid for the latency.
    auto engine = cas::system::create<actor::price_engine>();
    cas::system::start();

    heading("Sending ticks");
    //-----------------------
    constexpr int tick_total = 100;
    const auto begin = std::chrono::steady_clock::now();
    for (int i = 0; i < tick_total; ++i) {
        msg::market_tick tick;
        tick.price = 100.0 + i;
        engine.tell(tick);
    }
    ok &= check(wait_until(actor::price_engine::s_tick_count, tick_total),
                "the fast actor processed all 100 ticks");

    // Printed, not asserted: a timing threshold would fail on a loaded machine
    // and teach nothing. Measure on your own hardware.
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - begin);
    std::cout << "  100 ticks sent and processed in " << us.count() << " us\n";

    heading("Shut down");
    //-------------------
    cas::system::shutdown();
    cas::system::wait_for_shutdown();
    ok &= check(actor::price_engine::s_tick_count.load() == tick_total, "no ticks were lost");
    return ok;
}

} // namespace tut
