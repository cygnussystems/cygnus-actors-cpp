#include "../test_common.h"
#include "cas/detail/mailbox.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

// Level 14: the actor mailbox (cas::detail::mailbox)
//
// Custom lock-free code, adapted from CAF's lifo_inbox/default_mailbox. The
// project rule for such code is a dedicated stress test; these exercise the
// properties the scheduler relies on:
//  - exactly-once delivery and FIFO order per producer thread
//  - the blocked state: every push that finds the owner blocked reports
//    "unblocked", so a consumer that sleeps whenever it blocks never misses
//    a message (a lost wake-up hangs, and fails on the timeout)
//  - close() racing pushes: every message is consumed, dropped at close, or
//    handed back to its sender - exactly one of the three, none leaked
//  - ask requests are delivered in order with other messages (no priority)

namespace mailbox_test {

std::atomic<int> g_live{0};  // messages currently alive

struct seq_msg : cas::message_base {
    int producer = 0;
    int seq = 0;
    seq_msg(int p, int s) : producer(p), seq(s) { g_live.fetch_add(1); }
    seq_msg(const seq_msg& o) : cas::message_base(o), producer(o.producer), seq(o.seq) { g_live.fetch_add(1); }
    ~seq_msg() override { g_live.fetch_sub(1); }
};

struct probe_op {};

unsigned producer_count() {
    return std::max(4u, std::thread::hardware_concurrency() - 1);
}

} // namespace mailbox_test

using namespace mailbox_test;

TEST_CASE("Mailbox delivers exactly once, in order per producer", "[14_mailbox][stress]") {
    g_live = 0;
    {
        cas::detail::mailbox box;
        const unsigned producers = producer_count();
        constexpr int per_producer = 100'000;
        std::atomic<bool> go{false};
        std::atomic<int> unexpected_closed{0};  // Catch2 assertions are not thread-safe

        std::vector<std::thread> threads;
        for (unsigned p = 0; p < producers; ++p) {
            threads.emplace_back([&, p] {
                while (!go.load()) std::this_thread::yield();
                for (int s = 0; s < per_producer; ++s) {
                    std::unique_ptr<cas::message_base> m = std::make_unique<seq_msg>(int(p), s);
                    if (box.push(m) == cas::detail::push_result::closed) unexpected_closed.fetch_add(1);
                }
            });
        }

        std::vector<int> next_expected(producers, 0);
        uint64_t received = 0, out_of_order = 0;
        const uint64_t total = uint64_t(producers) * per_producer;
        go = true;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (received < total && std::chrono::steady_clock::now() < deadline) {
            if (auto m = box.pop()) {
                auto& s = static_cast<seq_msg&>(*m);
                if (s.seq != next_expected[s.producer]) ++out_of_order;
                next_expected[s.producer] = s.seq + 1;
                ++received;
            }
        }
        for (auto& t : threads) t.join();

        REQUIRE(unexpected_closed.load() == 0);
        REQUIRE(received == total);
        REQUIRE(out_of_order == 0);
        REQUIRE(box.size() == 0);
        REQUIRE(box.pop() == nullptr);
    }
    REQUIRE(g_live.load() == 0);
}

TEST_CASE("Mailbox reports unblocked so a sleeping consumer never misses a message", "[14_mailbox][stress]") {
    g_live = 0;
    {
        cas::detail::mailbox box;
        REQUIRE(box.try_block());  // start idle, as an actor with nothing to do

        const unsigned producers = producer_count();
        constexpr int per_producer = 20'000;
        std::atomic<int> wake_signals{0};  // one per "unblocked" push
        std::atomic<bool> go{false};

        std::vector<std::thread> threads;
        for (unsigned p = 0; p < producers; ++p) {
            threads.emplace_back([&, p] {
                while (!go.load()) std::this_thread::yield();
                for (int s = 0; s < per_producer; ++s) {
                    std::unique_ptr<cas::message_base> m = std::make_unique<seq_msg>(int(p), s);
                    if (box.push(m) == cas::detail::push_result::unblocked) {
                        wake_signals.fetch_add(1);
                    }
                    if ((s & 255) == 0) std::this_thread::yield();  // let the consumer go idle often
                }
            });
        }

        // The consumer only proceeds past "blocked" when a producer reports it
        // unblocked it - exactly how the scheduler decides to run an actor.
        uint64_t received = 0;
        int wakes_consumed = 0;
        const uint64_t total = uint64_t(producers) * per_producer;
        bool lost_wakeup = false;
        go = true;
        while (received < total) {
            while (auto m = box.pop()) ++received;
            if (!box.try_block()) continue;  // more arrived meanwhile: keep consuming
            // Blocked. Wait for the unblocking push's signal.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (wake_signals.load() == wakes_consumed) {
                if (received == total) break;
                if (std::chrono::steady_clock::now() > deadline) { lost_wakeup = true; break; }
                std::this_thread::yield();
            }
            if (lost_wakeup) break;
            if (wake_signals.load() > wakes_consumed) ++wakes_consumed;
        }
        for (auto& t : threads) t.join();

        REQUIRE_FALSE(lost_wakeup);
        REQUIRE(received == total);
        INFO("idle-to-busy transitions: " << wake_signals.load());
        REQUIRE(wake_signals.load() >= 1);
    }
    REQUIRE(g_live.load() == 0);
}

TEST_CASE("Mailbox close accounts for every message exactly once", "[14_mailbox][stress]") {
    for (int round = 0; round < 20; ++round) {
        g_live = 0;
        std::atomic<uint64_t> pushed{0}, rejected{0};
        std::atomic<int> lost_ownership{0};
        uint64_t consumed = 0, dropped = 0;
        {
            cas::detail::mailbox box;
            const unsigned producers = producer_count();
            std::atomic<bool> stop{false};
            std::vector<std::thread> threads;
            for (unsigned p = 0; p < producers; ++p) {
                threads.emplace_back([&, p] {
                    int s = 0;
                    while (!stop.load(std::memory_order_relaxed)) {
                        std::unique_ptr<cas::message_base> m = std::make_unique<seq_msg>(int(p), s++);
                        pushed.fetch_add(1);
                        if (box.push(m) == cas::detail::push_result::closed) {
                            if (m == nullptr) lost_ownership.fetch_add(1);  // caller must keep it
                            rejected.fetch_add(1);
                        }
                    }
                });
            }
            // Consume for a while, then close while producers are still pushing
            for (int i = 0; i < 20'000; ++i) {
                if (box.pop()) ++consumed;
            }
            dropped = box.close([](cas::message_base&, bool) {});
            REQUIRE(box.closed());
            std::this_thread::sleep_for(std::chrono::milliseconds(2));  // pushes keep failing
            stop = true;
            for (auto& t : threads) t.join();
            REQUIRE(box.pop() == nullptr);
        }
        REQUIRE(lost_ownership.load() == 0);
        REQUIRE(consumed + dropped + rejected.load() == pushed.load());
        REQUIRE(g_live.load() == 0);
    }
}

TEST_CASE("Mailbox keeps ask requests in order with other messages", "[14_mailbox]") {
    // Asks are not prioritised: an ask must not overtake messages its sender
    // sent earlier, or a caller could observe state that ignores its own
    // earlier messages (INVARIANT 3).
    cas::detail::mailbox box;
    std::unique_ptr<cas::message_base> m0 = std::make_unique<seq_msg>(0, 0);
    box.push(m0);
    std::unique_ptr<cas::message_base> ask =
        std::make_unique<cas::ask_request<int, probe_op>>(probe_op{});
    box.push(ask);
    std::unique_ptr<cas::message_base> m1 = std::make_unique<seq_msg>(0, 1);
    box.push(m1);
    REQUIRE(box.ask_size() == 1);
    REQUIRE(box.other_size() == 2);

    auto first = box.pop();
    REQUIRE(static_cast<seq_msg&>(*first).seq == 0);
    auto second = box.pop();
    REQUIRE(dynamic_cast<cas::ask_request_base*>(second.get()) != nullptr);
    auto third = box.pop();
    REQUIRE(static_cast<seq_msg&>(*third).seq == 1);
    REQUIRE(box.pop() == nullptr);
    REQUIRE(box.size() == 0);
}
