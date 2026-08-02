#include "../test_common.h"
#include <vector>
#include <algorithm>

// Level 11: message ordering.
//
// doc/40_message_passing.md previously promised "guaranteed" FIFO order between
// a pair of actors. The mailbox is a moodycamel::ConcurrentQueue enqueued
// without a ProducerToken, which does not provide that guarantee across
// producers. The documentation has been corrected; this test pins down what is
// actually true so the doc and the code cannot drift apart again silently.

namespace ordering_test {
    struct seq_msg : public cas::message_base {
        int seq = 0;
    };

    class recorder_actor : public cas::actor {
    private:
        std::vector<int> m_received;
        mutable std::mutex m_mutex;

    public:
        std::vector<int> received() const {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_received;
        }

        size_t count() const {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_received.size();
        }

    protected:
        void on_start() override {
            set_name("recorder");
            handler<seq_msg>([this](const seq_msg& msg) {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_received.push_back(msg.seq);
            });
        }
    };
}

TEST_CASE("All messages from a single sender are delivered exactly once",
          "[11_concurrency][ordering]") {
    CAS_TEST_GUARD();
    using namespace ordering_test;

    constexpr int kCount = 2000;

    auto rec = cas::system::create<recorder_actor>();
    cas::system::start();
    wait_ms(50);

    auto ref = cas::actor_registry::get("recorder");
    REQUIRE(ref.is_valid());

    for (int i = 0; i < kCount; ++i) {
        seq_msg msg;
        msg.seq = i;
        ref.tell(msg);
    }

    auto& obj = rec.get_checked<recorder_actor>();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (obj.count() < static_cast<size_t>(kCount) &&
           std::chrono::steady_clock::now() < deadline) {
        wait_ms(10);
    }

    auto received = obj.received();

    // Delivery is reliable: nothing lost, nothing duplicated.
    REQUIRE(received.size() == static_cast<size_t>(kCount));

    std::vector<int> sorted = received;
    std::sort(sorted.begin(), sorted.end());
    std::vector<int> expected(kCount);
    for (int i = 0; i < kCount; ++i) expected[i] = i;
    REQUIRE(sorted == expected);

    TEST_CLEANUP();
}

TEST_CASE("Concurrent senders all get their messages delivered",
          "[11_concurrency][ordering]") {
    CAS_TEST_GUARD();
    using namespace ordering_test;

    constexpr int kThreads = 4;
    constexpr int kPerThread = 500;

    auto rec = cas::system::create<recorder_actor>();
    cas::system::start();
    wait_ms(50);

    auto ref = cas::actor_registry::get("recorder");
    REQUIRE(ref.is_valid());

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([ref, t, n = kPerThread]() {
            for (int i = 0; i < n; ++i) {
                seq_msg msg;
                msg.seq = t * n + i;
                ref.tell(msg);
            }
        });
    }
    for (auto& th : threads) th.join();

    auto& obj = rec.get_checked<recorder_actor>();
    constexpr int kTotal = kThreads * kPerThread;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (obj.count() < static_cast<size_t>(kTotal) &&
           std::chrono::steady_clock::now() < deadline) {
        wait_ms(10);
    }

    // No ordering assertion across producers - the queue does not promise it.
    // What IS guaranteed is that every message arrives exactly once.
    auto received = obj.received();
    REQUIRE(received.size() == static_cast<size_t>(kTotal));

    std::vector<int> sorted = received;
    std::sort(sorted.begin(), sorted.end());
    auto dup = std::adjacent_find(sorted.begin(), sorted.end());
    REQUIRE(dup == sorted.end());  // no duplicates

    TEST_CLEANUP();
}
