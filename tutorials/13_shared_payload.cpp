// Tutorial 13: Large payloads - share them, don't copy them
//
// TEACHES
//   - carrying a large payload in a message through std::shared_ptr<const T>
//   - what tell() copies: the message, and with it the pointer - not the data
//   - one payload sent to several actors: all of them read the same buffer
//   - keeping a payload after the handler returns
//   - who frees the payload: whoever lets go of it last
//
// ASSUMES
//   Tutorials 1-12, especially message_pool.
//
// THE IDEA
//   tell() copies the message into the receiver's mailbox. For a few fields
//   that is the fastest thing to do; for a 10'000-price ladder it is not. Put
//   the bulk data behind a shared_ptr: the message stays small and pooled,
//   and each send copies a pointer and bumps a reference count.

#include "tutorials.h"
#include "cas/cas.h"

#include <atomic>
#include <cstddef>
#include <iostream>
#include <memory>
#include <vector>

namespace {

// The messages.
namespace msg {

// The payload: bulk data, built once by whoever publishes it. It is not a
// message - it is what a message points to.
struct price_ladder {
    static inline std::atomic<int> s_freed_count{0};    // observed by the tutorial

    std::vector<double> prices;

    ~price_ladder() { s_freed_count.fetch_add(1); }
};

// The message. It carries a pointer to the ladder, so it is the same size
// whether the ladder holds ten prices or ten million.
//
// The pointer is to a *const* ladder. Every actor that receives the message
// reads the same ladder, each on its own thread; the framework keeps an actor
// to one thread at a time, but it cannot stop two actors reading data they
// share. const makes "nobody writes it after it is sent" a compile error
// instead of a data race.
struct snapshot : cas::message_base {
    int sequence = 0;
    std::shared_ptr<const price_ladder> ladder;
};

} // namespace msg

// The actors.
namespace actor {

// Reads the ladder and lets go of it when the handler returns.
class pricer : public cas::actor {
public:
    static inline std::atomic<int> s_snapshot_count{0};                  // observed by the tutorial
    static inline std::atomic<const msg::price_ladder*> s_seen{nullptr};

protected:
    void on_start() override {
        set_name("pricer");
        handler<msg::snapshot>(&pricer::on_snapshot);
    }

private:
    void on_snapshot(const msg::snapshot& _msg) {
        const msg::price_ladder& ladder = *_msg.ladder;
        std::cout << "  pricer   <- snapshot " << _msg.sequence << ": " << ladder.prices.size()
                  << " prices, best " << ladder.prices.front() << "\n";
        s_seen.store(_msg.ladder.get());
        s_snapshot_count.fetch_add(1);
    }
};

// Keeps the latest ladder after the handler returns, by copying the pointer
// into a member - again a reference count, not a copy of the prices.
class recorder : public cas::actor {
public:
    static inline std::atomic<int> s_snapshot_count{0};                  // observed by the tutorial
    static inline std::atomic<const msg::price_ladder*> s_seen{nullptr};

protected:
    void on_start() override {
        set_name("recorder");
        handler<msg::snapshot>(&recorder::on_snapshot);
    }

private:
    void on_snapshot(const msg::snapshot& _msg) {
        std::cout << "  recorder <- snapshot " << _msg.sequence << ", keeping it\n";
        // Replacing m_latest lets go of the previous ladder. If nobody else
        // still holds that one, it is freed right here, on this actor's thread.
        m_latest = _msg.ladder;
        s_seen.store(m_latest.get());
        s_snapshot_count.fetch_add(1);
    }

    std::shared_ptr<const msg::price_ladder> m_latest;
};

} // namespace actor

// Builds a ladder and hands it out as const: from here on nobody can change it.
std::shared_ptr<const msg::price_ladder> make_ladder(std::size_t levels, double best) {
    auto ladder = std::make_shared<msg::price_ladder>();
    ladder->prices.resize(levels);
    for (std::size_t i = 0; i < levels; ++i) {
        ladder->prices[i] = best + 0.01 * static_cast<double>(i);
    }
    return ladder;
}

} // namespace

namespace tut {

bool run_shared_payload() {
    bool ok = true;

    heading("A small message with a large payload");
    //-----------------------------------------------
    constexpr std::size_t levels = 10'000;
    std::shared_ptr<const msg::price_ladder> ladder = make_ladder(levels, 100.0);

    std::cout << "  the ladder holds  " << levels * sizeof(double) << " bytes of prices\n"
              << "  sizeof(msg::snapshot) = " << sizeof(msg::snapshot)
              << "  (header + int + shared_ptr)\n";
    ok &= check(cas::message_pool::size_class_for_payload(sizeof(msg::snapshot)) != 0,
                "the message fits a pool size class, however big the ladder is");

    heading("One ladder, two receivers");
    //-----------------------------------
    auto pricer = cas::system::create<actor::pricer>();
    auto recorder = cas::system::create<actor::recorder>();
    cas::system::start();
    cas::message_pool::reset_stats();

    // tell() copies the message for each receiver. Copying the message copies
    // the shared_ptr: the ladder gains a reference, its prices stay where
    // they are. (One receiver at a time only so their output does not mix.)
    msg::snapshot first;
    first.sequence = 1;
    first.ladder = ladder;
    pricer.tell(first);
    ok &= check(wait_until(actor::pricer::s_snapshot_count, 1), "the pricer got the snapshot");
    recorder.tell(first);
    ok &= check(wait_until(actor::recorder::s_snapshot_count, 1), "the recorder got the snapshot");
    ok &= check(actor::pricer::s_seen.load() == ladder.get(), "the pricer read main()'s ladder, not a copy");
    ok &= check(actor::recorder::s_seen.load() == ladder.get(), "so did the recorder");
    ok &= check(cas::message_pool::get_stats().heap_fallbacks == 0, "both messages were served by the pool");

    heading("Keeping a payload after the handler");
    //---------------------------------------------
    // main() lets go of its handle (and of the message that held one). The
    // pricer let go when its handler returned. The recorder still holds the
    // ladder, so it is still alive.
    ladder.reset();
    first.ladder.reset();
    ok &= check(msg::price_ladder::s_freed_count.load() == 0, "the ladder lives on in the recorder");

    heading("The last holder frees it");
    //----------------------------------
    // A new ladder replaces the recorder's old one. The recorder was the last
    // holder of the first ladder, so the first ladder is freed - on the
    // recorder's thread, inside its handler. A large payload's free can land
    // in a latency-sensitive handler; keep a reference elsewhere if that matters.
    msg::snapshot second;
    second.sequence = 2;
    second.ladder = make_ladder(levels, 101.0);
    recorder.tell(second);

    ok &= check(wait_until(actor::recorder::s_snapshot_count, 2), "the recorder got the second snapshot");
    ok &= check(wait_until(msg::price_ladder::s_freed_count, 1), "the first ladder was freed, exactly once");

    heading("Shut down");
    //-------------------
    cas::system::shutdown();
    cas::system::wait_for_shutdown();
    return ok;
}

} // namespace tut
