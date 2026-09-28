#include "../test_common.h"
#include <vector>
#include <algorithm>

// Level 11: message ordering - INVARIANT 3 (doc/INVARIANTS.md).
//
// The guarantee: if sending A happens-before sending B, and both go to the
// same actor, A is handled before B. So messages from one actor, or from one
// thread, are handled in the order they were sent. There is no ordering
// between different senders. The one exception is call(): a call is handled
// next, ahead of every ordinary message still queued - including ones its
// sender sent earlier.
//
// History: early docs promised FIFO, but the mailbox was a
// moodycamel::ConcurrentQueue used without producer tokens, and nothing
// enforced or tested order; the docs were then corrected to "no guarantee".
// The mailbox is now an intrusive LIFO stack reversed by its single consumer
// (cas::detail::mailbox), which orders every push by one CAS - these tests pin
// the guarantee down so it cannot quietly regress.

namespace ordering_test {
    struct seq_msg : public cas::message_base {
        int source = 0;  // which sender
        int seq = 0;     // position in that sender's sequence
    };

    struct count_op {};

    class recorder_actor : public cas::actor {
    private:
        std::vector<std::pair<int, int>> m_received;  // (source, seq)
        mutable std::mutex m_mutex;

    public:
        std::vector<std::pair<int, int>> received() const {
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
                m_received.emplace_back(msg.source, msg.seq);
            });
            call_handler<int, count_op>(&recorder_actor::handled_so_far);
        }

        int handled_so_far() {
            return static_cast<int>(count());
        }
    };

    struct send_sequence : public cas::message_base {
        cas::actor_ref target;
        int source = 0;
        int count = 0;
    };

    // Sends a numbered sequence from inside a handler - an actor as the sender.
    class sequence_sender : public cas::actor {
    protected:
        void on_start() override {
            handler<send_sequence>([](const send_sequence& msg) {
                for (int i = 0; i < msg.count; ++i) {
                    seq_msg m;
                    m.source = msg.source;
                    m.seq = i;
                    msg.target.tell(m);
                }
            });
        }
    };

    bool wait_for_count(const recorder_actor& rec, size_t n) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (rec.count() < n && std::chrono::steady_clock::now() < deadline) {
            wait_ms(5);
        }
        return rec.count() >= n;
    }

    // Every sender's messages arrive complete and in increasing seq order.
    void require_fifo_per_source(const std::vector<std::pair<int, int>>& received,
                                 int sources, int per_source) {
        std::vector<int> next(sources, 0);
        size_t out_of_order = 0;
        for (auto [source, seq] : received) {
            if (seq != next[source]) ++out_of_order;
            next[source] = seq + 1;
        }
        REQUIRE(out_of_order == 0);
        for (int s = 0; s < sources; ++s) {
            REQUIRE(next[s] == per_source);
        }
    }
}

TEST_CASE("Messages from one thread are handled in send order",
          "[11_concurrency][ordering]") {
    CAS_TEST_GUARD();
    using namespace ordering_test;

    constexpr int kCount = 5000;

    auto rec = cas::system::create<recorder_actor>();
    cas::system::start();

    for (int i = 0; i < kCount; ++i) {
        seq_msg msg;
        msg.seq = i;
        rec.tell(msg);
    }

    auto& obj = rec.get_checked<recorder_actor>();
    REQUIRE(wait_for_count(obj, kCount));
    auto received = obj.received();
    REQUIRE(received.size() == static_cast<size_t>(kCount));
    for (int i = 0; i < kCount; ++i) {
        REQUIRE(received[i].second == i);
    }
}

TEST_CASE("Concurrent threads each keep their own send order",
          "[11_concurrency][ordering]") {
    CAS_TEST_GUARD();
    using namespace ordering_test;

    constexpr int kThreads = 8;
    constexpr int kPerThread = 2000;

    auto rec = cas::system::create<recorder_actor>();
    cas::system::start();

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([rec, t]() {
            for (int i = 0; i < kPerThread; ++i) {
                seq_msg msg;
                msg.source = t;
                msg.seq = i;
                rec.tell(msg);
            }
        });
    }
    for (auto& th : threads) th.join();

    auto& obj = rec.get_checked<recorder_actor>();
    REQUIRE(wait_for_count(obj, kThreads * kPerThread));
    auto received = obj.received();
    REQUIRE(received.size() == static_cast<size_t>(kThreads * kPerThread));
    // No order is promised between threads; within each thread it is.
    require_fifo_per_source(received, kThreads, kPerThread);
}

TEST_CASE("Actors sending to one actor each keep their own send order",
          "[11_concurrency][ordering]") {
    CAS_TEST_GUARD();
    using namespace ordering_test;

    constexpr int kSenders = 8;
    constexpr int kPerSender = 2000;

    auto rec = cas::system::create<recorder_actor>();
    std::vector<cas::actor_ref> senders;
    for (int s = 0; s < kSenders; ++s) {
        senders.push_back(cas::system::create<sequence_sender>());
    }
    cas::system::start();

    for (int s = 0; s < kSenders; ++s) {
        send_sequence go;
        go.target = rec;
        go.source = s;
        go.count = kPerSender;
        senders[s].tell(go);
    }

    auto& obj = rec.get_checked<recorder_actor>();
    REQUIRE(wait_for_count(obj, kSenders * kPerSender));
    auto received = obj.received();
    REQUIRE(received.size() == static_cast<size_t>(kSenders * kPerSender));
    require_fifo_per_source(received, kSenders, kPerSender);
}

TEST_CASE("A call is handled ahead of messages already queued",
          "[11_concurrency][ordering]") {
    CAS_TEST_GUARD();
    using namespace ordering_test;

    // Queue a backlog, and a call behind it, before start() - so the only
    // thing deciding the order is the mailbox. The call must be handled first:
    // it reports that nothing else has been handled yet.
    auto rec = cas::system::create<recorder_actor>();
    constexpr int backlog = 1000;
    for (int i = 0; i < backlog; ++i) {
        seq_msg msg;
        msg.seq = i;
        rec.tell(msg);
    }

    std::atomic<int> answer{-1};
    std::thread asker([&] { answer = rec.call<int>(count_op{}); });  // blocks until handled
    std::this_thread::sleep_for(std::chrono::milliseconds(100));   // call is queued

    cas::system::start();
    asker.join();

    REQUIRE(answer.load() == 0);
    auto& obj = rec.get_checked<recorder_actor>();
    REQUIRE(wait_for_count(obj, backlog));  // and the backlog still arrives, in order
    auto received = obj.received();
    for (int i = 0; i < backlog; ++i) {
        REQUIRE(received[i].second == i);
    }
}
