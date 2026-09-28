#include "../test_common.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// Level 13: message pool correctness
//
// The pool's free list used to be a hand-rolled Treiber stack whose pop was
// open to ABA: with two threads that both allocate and free, a block could be
// handed to two owners at once, and the user data written into it corrupted
// the free list. It crashed a two-thread stress loop within ~80ms, but the
// suite never noticed - every test had a single allocating thread, which
// cannot trigger ABA. The first two cases here are that missing coverage.

namespace pool_test {

// Restores the global pool cap on scope exit, including when a REQUIRE throws.
struct max_pool_size_guard {
    size_t saved = cas::message_pool::get_max_pool_size();
    ~max_pool_size_guard() { cas::message_pool::set_max_pool_size(saved); }
};

struct alignas(64) cache_line_msg : cas::message_base {
    int64_t value = 0;
};

// Not over-aligned as far as the language is concerned (16 is the default new
// alignment on x64), so it is pooled - and the pool must honour it.
struct alignas(16) sixteen_aligned_msg : cas::message_base {
    int64_t value = 0;
};

std::atomic<int> g_aligned_seen{0};
std::atomic<int> g_misaligned_seen{0};

class aligned_receiver : public cas::actor {
protected:
    void on_start() override {
        handler<cache_line_msg>([](const cache_line_msg& msg) {
            if (reinterpret_cast<std::uintptr_t>(&msg) % alignof(cache_line_msg) == 0) {
                g_aligned_seen.fetch_add(1);
            } else {
                g_misaligned_seen.fetch_add(1);
            }
        });
    }
};

// Each thread allocates and fills blocks with a stamp unique to that
// allocation, holds them briefly, and verifies the stamp before freeing. A
// block handed to two owners shows up as a mismatched stamp (or as a crash,
// when the free list itself is corrupted).
uint64_t run_same_thread_stress(unsigned threads, std::chrono::milliseconds duration) {
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> corruptions{0};
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < threads; ++t) {
        workers.emplace_back([&, t] {
            uint64_t stamp = uint64_t(t + 1) << 40;
            constexpr size_t block = 64;
            void* held[4];
            while (!stop.load(std::memory_order_relaxed)) {
                for (auto& p : held) {
                    p = cas::message_pool::allocate(block);
                    auto* w = static_cast<uint64_t*>(p);
                    ++stamp;
                    for (size_t i = 0; i < block / 8; ++i) w[i] = stamp;
                }
                std::this_thread::yield();
                for (auto& p : held) {
                    auto* w = static_cast<uint64_t*>(p);
                    for (size_t i = 1; i < block / 8; ++i) {
                        if (w[i] != w[0]) { corruptions.fetch_add(1); break; }
                    }
                    cas::message_pool::deallocate(p, block);
                }
            }
        });
    }
    std::this_thread::sleep_for(duration);
    stop = true;
    for (auto& w : workers) w.join();
    return corruptions.load();
}

// The path real messages take: allocated on one thread, freed on another.
uint64_t run_handoff_stress(unsigned threads, std::chrono::milliseconds duration) {
    struct channel {
        std::mutex mutex;
        std::vector<void*> items;
    };
    std::vector<channel> channels(threads);
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> corruptions{0};
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < threads; ++t) {
        workers.emplace_back([&, t] {
            uint64_t stamp = uint64_t(t + 1) << 40;
            constexpr size_t block = 100;  // 128-byte class once the caller adds its header
            channel& out = channels[(t + 1) % threads];
            channel& in = channels[t];
            std::vector<void*> received;
            while (!stop.load(std::memory_order_relaxed)) {
                {
                    std::lock_guard<std::mutex> lock(out.mutex);
                    for (int k = 0; k < 8 && out.items.size() < 256; ++k) {
                        auto* w = static_cast<uint64_t*>(cas::message_pool::allocate(block));
                        ++stamp;
                        for (size_t i = 0; i < 12; ++i) w[i] = stamp;
                        out.items.push_back(w);
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(in.mutex);
                    received.swap(in.items);
                }
                for (void* p : received) {
                    auto* w = static_cast<uint64_t*>(p);
                    for (size_t i = 1; i < 12; ++i) {
                        if (w[i] != w[0]) { corruptions.fetch_add(1); break; }
                    }
                    cas::message_pool::deallocate(p, block);
                }
                received.clear();
            }
        });
    }
    std::this_thread::sleep_for(duration);
    stop = true;
    for (auto& w : workers) w.join();
    for (auto& c : channels) {
        for (void* p : c.items) cas::message_pool::deallocate(p, 100);
    }
    return corruptions.load();
}

} // namespace pool_test

using namespace pool_test;

TEST_CASE("Pool never hands one block to two owners (same-thread churn)", "[13_message_pool][stress]") {
    const unsigned threads = std::max(4u, std::thread::hardware_concurrency());
    REQUIRE(run_same_thread_stress(threads, std::chrono::milliseconds(500)) == 0);
}

TEST_CASE("Pool never hands one block to two owners (cross-thread handoff)", "[13_message_pool][stress]") {
    const unsigned threads = std::max(4u, std::thread::hardware_concurrency());
    REQUIRE(run_handoff_stress(threads, std::chrono::milliseconds(500)) == 0);
}

TEST_CASE("Over-aligned messages are allocated with their alignment", "[13_message_pool][alignment]") {
    CAS_TEST_GUARD();
    static_assert(alignof(cache_line_msg) > cas::message_pool::POOLED_ALIGNMENT);

    // Directly: many allocations, to rule out a lucky address
    std::vector<std::unique_ptr<cas::message_base>> msgs;
    for (int i = 0; i < 64; ++i) {
        msgs.push_back(std::make_unique<cache_line_msg>());
        REQUIRE(reinterpret_cast<std::uintptr_t>(msgs.back().get()) % alignof(cache_line_msg) == 0);
    }
    msgs.clear();  // deleted through message_base: exercises the aligned delete

    // Through tell(), which clones the message into the mailbox
    g_aligned_seen = 0;
    g_misaligned_seen = 0;
    auto receiver = cas::system::create<aligned_receiver>();
    cas::system::start();
    for (int i = 0; i < 32; ++i) {
        receiver.tell(cache_line_msg{});
    }
    for (int i = 0; i < 300 && g_aligned_seen.load() + g_misaligned_seen.load() < 32; ++i) {
        wait_ms(10);
    }
    REQUIRE(g_aligned_seen.load() == 32);
    REQUIRE(g_misaligned_seen.load() == 0);
}

TEST_CASE("Pooled messages have the default new alignment", "[13_message_pool][alignment]") {
    static_assert(alignof(sixteen_aligned_msg) <= cas::message_pool::POOLED_ALIGNMENT);
    static_assert(cas::message_pool::POOLED_ALIGNMENT == __STDCPP_DEFAULT_NEW_ALIGNMENT__);

    cas::message_pool::reset_stats();
    std::vector<std::unique_ptr<cas::message_base>> msgs;
    for (int i = 0; i < 64; ++i) {
        msgs.push_back(std::make_unique<sixteen_aligned_msg>());
        // Was 8 bytes off on MSVC, where max_align_t (which set the header
        // padding) is 8 but the default new alignment is 16.
        REQUIRE(reinterpret_cast<std::uintptr_t>(msgs.back().get()) %
                cas::message_pool::POOLED_ALIGNMENT == 0);
    }
    const auto stats = cas::message_pool::get_stats();
    REQUIRE(stats.pool_hits + stats.pool_misses >= 64);  // pooled, not bypassed
}

TEST_CASE("Pool cap frees surplus blocks to the heap", "[13_message_pool][cap]") {
    max_pool_size_guard guard;
    cas::message_pool::set_max_pool_size(8);
    cas::message_pool::reset_stats();

    // Free far more blocks than the cap plus this thread's cache can hold.
    // Uses the 512-byte class, which nothing else in this test touches.
    std::vector<void*> blocks;
    for (int i = 0; i < 200; ++i) blocks.push_back(cas::message_pool::allocate(500));
    for (void* p : blocks) cas::message_pool::deallocate(p, 500);

    REQUIRE(cas::message_pool::get_stats().pool_full_frees > 0);
}

TEST_CASE("Oversized allocations fall back to the heap", "[13_message_pool][stats]") {
    cas::message_pool::reset_stats();
    void* p = cas::message_pool::allocate(cas::message_pool::MAX_POOLED_SIZE + 1);
    cas::message_pool::deallocate(p, cas::message_pool::MAX_POOLED_SIZE + 1);
    REQUIRE(cas::message_pool::get_stats().heap_fallbacks == 1);
}
