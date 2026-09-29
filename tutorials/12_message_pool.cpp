// Tutorial 12: The message pool
//
// TEACHES
//   - that messages are pooled, not individually heap-allocated
//   - header overhead: why a message is bigger than its fields
//   - size classes, and the heap fallback for messages that outgrow them
//   - reading pool statistics, and prewarming
//
// ASSUMES
//   Tutorials 1-11, especially fixed_string.
//
// THE IDEA
//   Every send allocates a message and every delivery frees it. The framework
//   serves these from free lists of fixed-size blocks instead of the general
//   heap - free, as long as the message fits a size class. Worth checking.

#include "tutorials.h"
#include "cas/cas.h"

#include <atomic>
#include <iostream>
#include <string>

namespace {

class feed : public cas::actor {
public:
    // Small, with inline storage: lands in a size class and is pooled.
    struct tick : cas::message_base {
        cas::fixed_string<8> symbol;
        double price = 0.0;
    };

    static inline std::atomic<int> ticks{0};             // observed by the tutorial

protected:
    void on_start() override {
        set_name("feed");
        handler<tick>(&feed::on_tick);
    }

private:
    void on_tick(const tick&) { ticks.fetch_add(1); }
};

// Never sent - shown for contrast. The message itself is pooled, but the
// std::string member still allocates its characters on the heap; the pool
// cannot help with what a member allocates internally.
struct chatty : cas::message_base {
    std::string text;
};

} // namespace

namespace tut {

bool run_message_pool() {
    bool ok = true;

    heading("What a message actually costs");
    //---------------------------------------
    // message_base carries a sender, a message id, a correlation id and a
    // vtable pointer. That fixed overhead dominates small messages.
    std::cout << "  sizeof(cas::message_base) = " << sizeof(cas::message_base) << "\n"
              << "  sizeof(feed::tick)        = " << sizeof(feed::tick)
              << "  (header + fixed_string<8> + double)\n"
              << "  sizeof(chatty)            = " << sizeof(chatty)
              << "  (plus whatever the std::string allocates)\n";
    ok &= check(sizeof(feed::tick) > sizeof(cas::message_base), "a message is its header plus its fields");

    heading("Size classes");
    //----------------------
    // Allocations round up to the next class; anything bigger than the largest
    // falls back to the heap. Keep hot-path messages under a class boundary.
    const size_t needed = sizeof(feed::tick);
    const size_t assigned = cas::message_pool::size_class_for_payload(needed);
    std::cout << "  a " << needed << "-byte message uses the " << assigned << "-byte size class\n";
    ok &= check(assigned >= needed, "the size class is at least as big as the message");

    heading("Watching the pool while sending");
    //-----------------------------------------
    // Statistics tell a pooled allocation from a heap fallback: check, don't assume.
    cas::message_pool::reset_stats();
    auto feed_ref = cas::system::create<feed>();
    cas::system::start();

    constexpr int messages = 200;
    for (int i = 0; i < messages; ++i) {
        feed::tick t;
        t.symbol = "AAPL";
        t.price = 100.0 + i;
        feed_ref.tell(t);
    }
    ok &= check(wait_until(feed::ticks, messages), "all 200 messages were delivered");

    const auto stats = cas::message_pool::get_stats();
    std::cout << "  pool hits:      " << stats.pool_hits << "\n"
              << "  pool misses:    " << stats.pool_misses << "  (free list was empty and grew)\n"
              << "  heap fallbacks: " << stats.heap_fallbacks << "  (too big for any size class)\n";
    ok &= check(stats.heap_fallbacks == 0, "no message fell back to the heap");
    ok &= check(stats.pool_hits + stats.pool_misses > 0, "the pool served the allocations");

    heading("Prewarming");
    //--------------------
    // The first allocations in a class must create blocks. prewarm() does that
    // up front, so a latency-sensitive system does not pay for it after start.
    cas::message_pool::reset_stats();
    cas::message_pool::prewarm(64);
    for (int i = 0; i < 32; ++i) {
        feed::tick t;
        t.symbol = "MSFT";
        feed_ref.tell(t);
    }
    const auto warm = cas::message_pool::get_stats();
    std::cout << "  after prewarm(64): " << warm.pool_hits << " hits, " << warm.pool_misses << " misses\n";
    ok &= check(warm.pool_hits > 0, "prewarmed blocks served the following allocations");

    heading("Shut down");
    //-------------------
    cas::system::shutdown();
    cas::system::wait_for_shutdown();
    return ok;
}

} // namespace tut
