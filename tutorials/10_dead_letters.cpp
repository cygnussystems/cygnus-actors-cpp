// Tutorial 10: Dead letters
//
// TEACHES
//   - what happens to a message that cannot be delivered
//   - get_dead_letter_stats() to count them
//   - set_dead_letter_handler() to inspect each one
//   - why silent dropping is the default, and how to notice it
//
// ASSUMES
//   Tutorials 1-9.
//
// THE IDEA
//   Tutorial 8 showed a message to a stopped actor vanishing with no error.
//   That is deliberate - a send is fire-and-forget and must not block or
//   throw - but it means a bug can hide as silence. Dead letter tracking is
//   how you make the silence visible.

#include "tutorials.h"
#include "cas/cas.h"

#include <atomic>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

namespace {

struct payload : cas::message_base {
    int id = 0;
};

std::atomic<int> g_delivered{0};

// Records what the dead letter handler saw. The handler may be invoked from
// another thread, so this needs a lock.
std::mutex g_dead_mutex;
std::vector<std::string> g_dead_letter_names;

// Dead letters are counted by category rather than as one total: a dropped
// tell() and a dropped ask() are different problems. This sums them for the
// checks below.
size_t total_dead_letters() {
    const auto s = cas::system::get_dead_letter_stats();
    return s.dropped_tell + s.dropped_ask + s.dropped_to_invalid;
}

class receiver : public cas::actor {
protected:
    void on_start() override {
        set_name("receiver");
        handler<payload>(&receiver::on_payload);
    }

    void on_payload(const payload& msg) {
        std::cout << "  receiver got payload " << msg.id << "\n";
        g_delivered.fetch_add(1);
    }
};

} // namespace

namespace tut {

bool run_dead_letters() {
    bool ok = true;

    section("Starting");
    auto receiver_ref = cas::system::create<receiver>();
    cas::system::start();

    // Baseline: a normal delivery is not a dead letter.
    payload good;
    good.id = 1;
    receiver_ref.tell(good);
    ok &= check(wait_until([] { return g_delivered.load() == 1; }),
                "a normal message was delivered");

    // --- 1. Counting undeliverable messages ----------------------------------
    //
    // Dead letter statistics are always collected; nothing needs enabling.

    section("Sending to a stopped actor");
    cas::system::reset_dead_letter_stats();

    cas::system::stop_actor(receiver_ref);

    payload lost;
    lost.id = 2;
    receiver_ref.tell(lost);  // nowhere to go

    ok &= check(wait_until([] { return total_dead_letters() > 0; }),
                "the undeliverable message was counted as a dead letter");

    const auto stats = cas::system::get_dead_letter_stats();
    std::cout << "  dropped tells: " << stats.dropped_tell
              << ", dropped asks: " << stats.dropped_ask
              << ", to invalid refs: " << stats.dropped_to_invalid << "\n";

    // --- 2. Inspecting each one ----------------------------------------------
    //
    // A handler gives the detail behind the count: which actor, which message
    // type, what state the actor was in. Useful in development to catch a
    // wrongly-ordered shutdown, and in production to alert on message loss.
    //
    // Called on whichever thread discovered the failure, so anything it
    // touches must be thread-safe. Keep it short - it runs on the send path.

    section("Installing a dead letter handler");
    cas::system::set_dead_letter_handler([](const cas::dead_letter_info& info) {
        std::lock_guard<std::mutex> lock(g_dead_mutex);
        g_dead_letter_names.push_back(info.target_actor_name);
        std::cout << "  [dead letter] to '" << info.target_actor_name << "'\n";
    });

    payload lost_again;
    lost_again.id = 3;
    receiver_ref.tell(lost_again);

    ok &= check(wait_until([] {
                    std::lock_guard<std::mutex> lock(g_dead_mutex);
                    return !g_dead_letter_names.empty();
                }),
                "the handler was called for the undeliverable message");

    {
        std::lock_guard<std::mutex> lock(g_dead_mutex);
        const bool named = !g_dead_letter_names.empty() &&
                           g_dead_letter_names.front() == "receiver";
        ok &= check(named, "the handler reported the intended recipient's name");
    }

    // --- 3. Clearing up ------------------------------------------------------
    //
    // The handler is global and outlives any one actor, so remove it when it
    // is no longer wanted - especially in tests, where a stale handler
    // capturing local state would outlive that state.

    section("Removing the handler");
    cas::system::clear_dead_letter_handler();

    const size_t before = g_dead_letter_names.size();
    payload after_clear;
    after_clear.id = 4;
    receiver_ref.tell(after_clear);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    {
        std::lock_guard<std::mutex> lock(g_dead_mutex);
        ok &= check(g_dead_letter_names.size() == before,
                    "the handler stopped being called after clearing it");
    }

    // The statistics keep counting regardless - they are not tied to the
    // handler.
    ok &= check(total_dead_letters() >= 3,
                "statistics still counted the drops after the handler was cleared");

    section("Shutting down");
    cas::system::shutdown();
    cas::system::wait_for_shutdown();

    return ok;
}

} // namespace tut
