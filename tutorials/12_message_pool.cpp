// Tutorial 12: The message pool
//
// TEACHES
//   - that messages are pooled, not individually heap-allocated
//   - header overhead, and why a message is bigger than its fields
//   - size classes, and what happens when a message outgrows the largest
//   - reading pool statistics to check a message is actually being pooled
//
// ASSUMES
//   Tutorials 1-11, especially fixed_string.
//
// THE IDEA
//   Every message sent is allocated and freed. Doing that through the general
//   heap at high message rates is slow and fragments memory, so the framework
//   keeps free lists of fixed-size blocks. You get this for free - but only
//   while messages fit in a size class, which is worth being able to check.

#include "tutorials.h"
#include "cas/cas.h"

#include <atomic>
#include <iostream>
#include <string>

namespace {

// --- 1. A message sized to the pool ------------------------------------------
//
// Small, with inline storage. This lands in a size class and is pooled.

struct tick : cas::message_base {
    cas::fixed_string<8> symbol;
    double price = 0.0;
};

// --- 2. A message that defeats the pool --------------------------------------
//
// The message header itself is pooled, but a std::string member keeps its
// characters on the heap - so sending one still costs a heap allocation. The
// pool cannot help with what a member allocates internally.
struct chatty : cas::message_base {
    std::string text;  // allocates outside the pool
};

std::atomic<int> g_ticks{0};

class feed : public cas::actor {
protected:
    void on_start() override {
        set_name("feed");
        handler<tick>(&feed::on_tick);
    }

    void on_tick(const tick&) {
        g_ticks.fetch_add(1);
    }
};

} // namespace

namespace tut {

bool run_message_pool() {
    bool ok = true;

    // --- 3. Header overhead --------------------------------------------------
    //
    // A message is bigger than the fields you declared. message_base carries a
    // sender reference, a message id and a correlation id, plus a vtable
    // pointer because it is polymorphic. That overhead is the same for every
    // message, so it dominates small ones.

    section("What a message actually costs");
    std::cout << "  sizeof(cas::message_base) = " << sizeof(cas::message_base) << "\n";
    std::cout << "  sizeof(tick)              = " << sizeof(tick)
              << "  (fixed_string<8> + double + header)\n";
    std::cout << "  sizeof(chatty)            = " << sizeof(chatty)
              << "  (std::string member, plus whatever it allocates)\n";

    ok &= check(sizeof(tick) > sizeof(cas::message_base),
                "a message is its header plus its fields");

    // --- 4. Size classes -----------------------------------------------------
    //
    // The pool keeps blocks in a few fixed sizes. An allocation is rounded up
    // to the next class; anything larger than the biggest falls back to the
    // heap. Keeping a hot-path message under a class boundary is the practical
    // reason to care about its size.

    section("Size classes");
    const size_t needed = sizeof(tick);
    const size_t assigned = cas::message_pool::size_class_for_payload(needed);
    std::cout << "  a " << needed << "-byte message uses the "
              << assigned << "-byte size class\n";
    ok &= check(assigned >= needed, "the size class is at least as big as the message");

    // --- 5. Proving messages are pooled --------------------------------------
    //
    // Statistics distinguish a pooled allocation from a heap fallback, which
    // is how you check a message really is on the fast path rather than
    // assuming it.

    section("Watching the pool while sending");
    cas::message_pool::reset_stats();

    auto feed_ref = cas::system::create<feed>();
    cas::system::start();

    constexpr int k_messages = 200;
    for (int i = 0; i < k_messages; ++i) {
        tick t;
        t.symbol = "AAPL";
        t.price = 100.0 + i;
        feed_ref.tell(t);
    }

    ok &= check(wait_until([] { return g_ticks.load() == k_messages; }),
                "all 200 messages were delivered");

    const auto stats = cas::message_pool::get_stats();
    std::cout << "  pool hits:      " << stats.pool_hits << "\n";
    std::cout << "  pool misses:    " << stats.pool_misses
              << "  (a miss just means the free list was empty and grew)\n";
    std::cout << "  heap fallbacks: " << stats.heap_fallbacks
              << "  (messages too big for any size class)\n";

    ok &= check(stats.heap_fallbacks == 0,
                "no message fell back to the heap - all fitted a size class");
    ok &= check(stats.pool_hits + stats.pool_misses > 0,
                "the pool served the message allocations");

    // --- 6. Prewarming -------------------------------------------------------
    //
    // The first allocation in each class has to create blocks. prewarm()
    // does that up front, so a latency-sensitive system does not pay it on
    // the first messages after start.

    section("Prewarming");
    cas::message_pool::reset_stats();
    cas::message_pool::prewarm(64);

    // With blocks already on the free lists, the next allocations are hits
    // rather than misses.
    for (int i = 0; i < 32; ++i) {
        tick t;
        t.symbol = "MSFT";
        feed_ref.tell(t);
    }

    const auto warm = cas::message_pool::get_stats();
    std::cout << "  after prewarm(64): " << warm.pool_hits << " hits, "
              << warm.pool_misses << " misses\n";
    ok &= check(warm.pool_hits > 0,
                "prewarmed blocks served the following allocations");

    section("Shutting down");
    cas::system::shutdown();
    cas::system::wait_for_shutdown();

    return ok;
}

} // namespace tut
