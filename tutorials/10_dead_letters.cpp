// Tutorial 10: Dead letters
//
// TEACHES
//   - what happens to a message that cannot be delivered
//   - get_dead_letter_stats() to count them
//   - set_dead_letter_handler() / clear_dead_letter_handler() to inspect each
//
// ASSUMES
//   Tutorials 1-9.
//
// THE IDEA
//   A send is fire-and-forget and must not block or throw, so a message to a
//   stopped actor vanishes silently (tutorial 8). Dead letter tracking makes
//   that silence visible.

#include "tutorials.h"
#include "cas/cas.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

class receiver : public cas::actor {
public:
    struct payload : cas::message_base {
        int id = 0;
    };

    static inline std::atomic<int> delivered{0};         // observed by the tutorial

protected:
    void on_start() override {
        set_name("receiver");
        handler<payload>(&receiver::on_payload);
    }

private:
    void on_payload(const payload& msg) {
        std::cout << "  receiver got payload " << msg.id << "\n";
        delivered.fetch_add(1);
    }
};

// What the tutorial's dead letter handler records. The handler runs on
// whichever thread discovered the failure, so access is locked.
struct dead_letter_log {
    static void record(const cas::dead_letter_info& info) {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_targets.push_back(info.target_actor_name);
        std::cout << "  [dead letter] to '" << info.target_actor_name << "'\n";
    }

    static std::vector<std::string> targets() {
        std::lock_guard<std::mutex> lock(s_mutex);
        return s_targets;
    }

    // Stats are kept per category - a dropped tell() and a dropped call() are
    // different problems. This sums them.
    static std::size_t total_dropped() {
        const auto s = cas::system::get_dead_letter_stats();
        return s.dropped_tell + s.dropped_ask + s.dropped_to_invalid;
    }

private:
    static inline std::mutex s_mutex;
    static inline std::vector<std::string> s_targets;
};

void send_payload(const cas::actor_ref& to, int id) {
    receiver::payload msg;
    msg.id = id;
    to.tell(msg);
}

} // namespace

namespace tut {

bool run_dead_letters() {
    bool ok = true;

    heading("Starting");
    auto receiver_ref = cas::system::create<receiver>();
    cas::system::start();
    send_payload(receiver_ref, 1);
    ok &= check(wait_until([] { return receiver::delivered.load() == 1; }), "a normal message was delivered");

    heading("Sending to a stopped actor");
    // Statistics are always collected; nothing needs enabling.
    cas::system::reset_dead_letter_stats();
    cas::system::stop_actor(receiver_ref);
    send_payload(receiver_ref, 2);                       // nowhere to go
    ok &= check(wait_until([] { return dead_letter_log::total_dropped() > 0; }),
                "the undeliverable message was counted as a dead letter");
    const auto stats = cas::system::get_dead_letter_stats();
    std::cout << "  dropped tells: " << stats.dropped_tell << ", dropped calls: " << stats.dropped_ask
              << ", to invalid refs: " << stats.dropped_to_invalid << "\n";

    heading("Installing a dead letter handler");
    // The handler gives the detail behind the count: which actor, which
    // message type, what state. It runs on the send path - keep it short and
    // thread-safe.
    cas::system::set_dead_letter_handler(&dead_letter_log::record);
    send_payload(receiver_ref, 3);
    ok &= check(wait_until([] { return !dead_letter_log::targets().empty(); }),
                "the handler was called for the undeliverable message");
    ok &= check(dead_letter_log::targets().front() == "receiver",
                "the handler reported the intended recipient's name");

    heading("Removing the handler");
    // The handler is global and outlives any actor: remove it when done.
    cas::system::clear_dead_letter_handler();
    const std::size_t before = dead_letter_log::targets().size();
    send_payload(receiver_ref, 4);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ok &= check(dead_letter_log::targets().size() == before,
                "the handler stopped being called after clearing it");
    // The statistics are independent of the handler.
    ok &= check(dead_letter_log::total_dropped() >= 3,
                "statistics still counted the drops after the handler was cleared");

    heading("Shutting down");
    cas::system::shutdown();
    cas::system::wait_for_shutdown();
    return ok;
}

} // namespace tut
