// Tutorial 13: Fast actors
//
// TEACHES
//   - fast_actor, which runs on its own dedicated thread
//   - the polling strategies and what each trades away
//   - when a dedicated thread is worth it, and when it is waste
//
// ASSUMES
//   Tutorials 1-12.
//
// THE IDEA
//   Ordinary actors share a pool of worker threads. That is efficient - you
//   can have thousands - but a message may wait for a worker to pick it up.
//   A fast_actor gets a thread to itself that polls its mailbox continuously,
//   trading CPU for latency.
//
//   This is a specialist tool. A dedicated thread per actor does not scale,
//   so use it for the few actors on the critical path and leave the rest
//   pooled.

#include "tutorials.h"
#include "cas/cas.h"

#include <atomic>
#include <chrono>
#include <iostream>

namespace {

struct market_tick : cas::message_base {
    double price = 0.0;
};

std::atomic<int> g_ticks_processed{0};
std::atomic<long long> g_last_latency_ns{0};

// --- A fast actor ------------------------------------------------------------
//
// Derives from cas::fast_actor instead of cas::actor. Everything else - the
// handlers, on_start(), the messages - is identical. Only the execution model
// changes.

class price_engine : public cas::fast_actor {
protected:
    void on_start() override {
        set_name("price_engine");
        handler<market_tick>(&price_engine::on_tick);

        // --- Polling strategy ------------------------------------------------
        //
        //   yield      - give the CPU up when idle. Low latency (~1-10us) and
        //                a good neighbour. The default, and the right choice
        //                unless measurement says otherwise.
        //
        //   hybrid     - spin briefly, then yield. Sub-microsecond latency at
        //                moderate CPU cost.
        //
        //   busy_wait  - never yield. Lowest possible latency and a core
        //                pinned at 100% for as long as the actor lives. Only
        //                justified with a core to spare and latency that
        //                genuinely matters.
        //
        // This tutorial uses yield: it should not burn a core to print a few
        // lines, and neither should most systems.
        set_polling_strategy(cas::polling_strategy::yield);
    }

    void on_tick(const market_tick& msg) {
        (void)msg;
        g_ticks_processed.fetch_add(1);
    }
};

} // namespace

namespace tut {

bool run_fast_actor() {
    bool ok = true;

    section("Starting a fast actor");
    auto engine = cas::system::create<price_engine>();
    cas::system::start();

    // The actor now owns a thread that polls its mailbox. It is running even
    // with nothing to do - that is the cost being paid for the latency.

    section("Sending ticks");
    constexpr int k_ticks = 100;

    const auto begin = std::chrono::steady_clock::now();
    for (int i = 0; i < k_ticks; ++i) {
        market_tick t;
        t.price = 100.0 + i;
        engine.tell(t);
    }

    ok &= check(wait_until([] { return g_ticks_processed.load() == k_ticks; }),
                "the fast actor processed all 100 ticks");

    const auto elapsed = std::chrono::steady_clock::now() - begin;
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(elapsed);
    std::cout << "  100 ticks sent and processed in " << us.count() << " us\n";

    // Deliberately not asserting a latency figure. A timing threshold in a
    // tutorial would fail on a loaded or virtualised machine and teach
    // nothing; measure on your own hardware, with your own message sizes.

    // --- Choosing ------------------------------------------------------------
    //
    // Use fast_actor when an actor is on a latency-critical path and you can
    // spare a core for it: a market data feed, a control loop, a game tick.
    //
    // Stay with a pooled actor for everything else. Fast actors do not scale -
    // each one holds a thread whether or not it has work - and a system with
    // more of them than cores will run slower than the pooled version, not
    // faster.

    section("Shutting down");
    cas::system::shutdown();
    cas::system::wait_for_shutdown();

    ok &= check(g_ticks_processed.load() == k_ticks,
                "no ticks were lost");

    return ok;
}

} // namespace tut
