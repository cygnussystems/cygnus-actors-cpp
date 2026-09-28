#include "cas/cas.h"
#include <chrono>
#include <iostream>
#include <atomic>
#include <iomanip>
#include <cstdlib>
#include <vector>

// Global counters for benchmark coordination
static std::atomic<int64_t> g_received{0};
static std::atomic<bool> g_done{false};
static int64_t g_expected = 0;

// Simple message for throughput test
struct bench_message : cas::message_base {
    int64_t sequence;
};

// Message with std::string (heap allocation per string)
struct string_message : cas::message_base {
    std::string symbol;
    std::string client_id;
    int64_t quantity;
    int64_t price;
    int64_t sequence;
};

// Message with fixed_string (zero allocation)
struct fixed_message : cas::message_base {
    cas::fixed_string<16> symbol;      // Fits "AAPL.NASDAQ.US" (14 chars)
    cas::fixed_string<32> client_id;   // Fits "client-001-trading-desk-alpha" (29 chars)
    int64_t quantity;
    int64_t price;
    int64_t sequence;
};

// Receiver actor - counts messages using global counter
class receiver_actor : public cas::actor {
protected:
    void on_start() override {
        set_name("receiver");
        handler<bench_message>(&receiver_actor::on_bench);
        handler<string_message>(&receiver_actor::on_string);
        handler<fixed_message>(&receiver_actor::on_fixed);
    }

    void on_bench(const bench_message& msg) {
        if (++g_received >= g_expected) {
            g_done = true;
        }
    }

    void on_string(const string_message& msg) {
        if (++g_received >= g_expected) {
            g_done = true;
        }
    }

    void on_fixed(const fixed_message& msg) {
        if (++g_received >= g_expected) {
            g_done = true;
        }
    }
};

template<typename MessageType>
double run_throughput_test(const std::string& test_name, int64_t num_messages) {
    // Reset globals
    g_received = 0;
    g_done = false;
    g_expected = num_messages;

    // Create actor FIRST, then start the system
    auto receiver = cas::system::create<receiver_actor>();

    cas::system::start();

    // Wait for actor to be ready
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Get actor ref from registry
    auto receiver_ref = cas::actor_registry::get("receiver");

    auto start = std::chrono::high_resolution_clock::now();

    // Send all messages
    for (int64_t i = 0; i < num_messages; ++i) {
        MessageType msg;
        msg.sequence = i;
        if constexpr (std::is_same_v<MessageType, string_message> ||
                      std::is_same_v<MessageType, fixed_message>) {
            // Use strings longer than SSO threshold (~15 chars on MSVC)
            // to force heap allocation for std::string
            msg.symbol = "AAPL.NASDAQ.US";           // 14 chars - near SSO limit
            msg.client_id = "client-001-trading-desk-alpha";  // 29 chars - defeats SSO
            msg.quantity = 100;
            msg.price = 15000;
        }
        receiver_ref.tell(msg);
    }

    // Wait for all messages to be processed (with timeout)
    auto wait_start = std::chrono::steady_clock::now();
    while (!g_done) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        auto elapsed = std::chrono::steady_clock::now() - wait_start;
        if (elapsed > std::chrono::seconds(30)) {
            std::cerr << "TIMEOUT waiting for messages. Received: " << g_received << "/" << g_expected << "\n";
            break;
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    double seconds = duration.count() / 1'000'000.0;
    double msgs_per_sec = g_received / seconds;

    std::cout << std::fixed << std::setprecision(2);
    std::cout << test_name << ":\n";
    std::cout << "  Messages:    " << g_received.load() << "\n";
    std::cout << "  Time:        " << duration.count() / 1000.0 << " ms\n";
    std::cout << "  Throughput:  " << msgs_per_sec / 1'000'000.0 << " M msg/sec\n";
    std::cout << "  Latency:     " << (duration.count() * 1000.0 / g_received) << " ns/msg (avg)\n";
    std::cout << "\n";

    cas::system::shutdown();
    cas::system::wait_for_shutdown();

    return msgs_per_sec;
}

// --- Many-to-many: actors sending to actors -------------------------------
//
// The scenarios above send from main(), a single non-actor thread. Real
// systems send from actor handlers on many worker threads at once, which is
// where contention on anything shared per message (counters, reference
// counts, wakeups) shows up. Here N sender actors each spread their messages
// across all N receiver actors.
//
// Receipts are counted per receiver, on separate cache lines, so the
// benchmark's own bookkeeping does not add the very contention it measures.

struct alignas(64) padded_counter {
    std::atomic<int64_t> value{0};
};

struct start_sending : cas::message_base {
    std::vector<cas::actor_ref> receivers;
    int64_t count = 0;
};

class counting_receiver : public cas::actor {
public:
    explicit counting_receiver(padded_counter* counter) : m_counter(counter) {}

protected:
    void on_start() override {
        handler<bench_message>(&counting_receiver::on_bench);
    }

    void on_bench(const bench_message&) {
        m_counter->value.fetch_add(1, std::memory_order_relaxed);
    }

private:
    padded_counter* m_counter;
};

class spreading_sender : public cas::actor {
protected:
    void on_start() override {
        handler<start_sending>(&spreading_sender::on_start_sending);
    }

    void on_start_sending(const start_sending& msg) {
        const size_t n = msg.receivers.size();
        for (int64_t i = 0; i < msg.count; ++i) {
            bench_message m;
            m.sequence = i;
            msg.receivers[static_cast<size_t>(i) % n].tell(m);
        }
    }
};

double run_many_to_many_test(const std::string& test_name, int actors, int64_t messages_per_sender) {
    cas::system::reset();  // start from an empty system: earlier scenarios leave actors behind

    std::vector<padded_counter> counters(actors);
    std::vector<cas::actor_ref> receivers;
    std::vector<cas::actor_ref> senders;
    for (int i = 0; i < actors; ++i) receivers.push_back(cas::system::create<counting_receiver>(&counters[i]));
    for (int i = 0; i < actors; ++i) senders.push_back(cas::system::create<spreading_sender>());

    cas::system::start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const int64_t expected = messages_per_sender * actors;
    auto received = [&] {
        int64_t sum = 0;
        for (auto& c : counters) sum += c.value.load(std::memory_order_relaxed);
        return sum;
    };

    auto start = std::chrono::high_resolution_clock::now();

    for (auto& sender : senders) {
        start_sending go;
        go.receivers = receivers;
        go.count = messages_per_sender;
        sender.tell(go);
    }

    auto wait_start = std::chrono::steady_clock::now();
    while (received() < expected) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (std::chrono::steady_clock::now() - wait_start > std::chrono::seconds(30)) {
            std::cerr << "TIMEOUT waiting for messages. Received: " << received() << "/" << expected << "\n";
            break;
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    const int64_t total = received();
    const double msgs_per_sec = total / (duration.count() / 1'000'000.0);

    std::cout << std::fixed << std::setprecision(2);
    std::cout << test_name << ":\n";
    std::cout << "  Messages:    " << total << "\n";
    std::cout << "  Time:        " << duration.count() / 1000.0 << " ms\n";
    std::cout << "  Throughput:  " << msgs_per_sec / 1'000'000.0 << " M msg/sec\n";
    std::cout << "\n";

    cas::system::shutdown();
    cas::system::wait_for_shutdown();
    cas::system::reset();

    return msgs_per_sec;
}

int main(int argc, char** argv) {
    std::cout << "=== Cygnus Actor Framework Benchmark ===\n\n";

    // Optional: pooled workers' idle spin in microseconds (default: library default)
    if (argc > 1) {
        cas::system_config config;
        config.worker_idle_spin = std::chrono::microseconds(std::atoi(argv[1]));
        cas::system::configure(config);
        std::cout << "worker_idle_spin = " << config.worker_idle_spin.count() << " us\n\n";
    }
    std::cout << std::flush;

    constexpr int64_t NUM_MESSAGES = 1'000'000;

    // Test 1: Minimal message (baseline)
    std::cout << "--- Baseline ---\n\n" << std::flush;
    run_throughput_test<bench_message>(
        "Minimal message (int64 only)", NUM_MESSAGES);

    // Test 2: Compare std::string vs fixed_string
    std::cout << "--- String Comparison (same fields) ---\n\n" << std::flush;
    run_throughput_test<string_message>(
        "std::string fields (heap alloc)", NUM_MESSAGES);

    run_throughput_test<fixed_message>(
        "fixed_string fields (zero alloc)", NUM_MESSAGES);

    // Test 3: Many senders to many receivers, all actors
    std::cout << "--- Actors to actors (concurrent senders) ---\n\n" << std::flush;
    run_many_to_many_test("8 senders -> 8 receivers", 8, 500'000);

    std::cout << "=== Benchmark Complete ===\n";

    return 0;
}
