#include "cas/cas.h"
#include <chrono>
#include <iostream>
#include <atomic>
#include <iomanip>

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

int main() {
    std::cout << "=== Cygnus Actor Framework Benchmark ===\n\n";
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

    std::cout << "=== Benchmark Complete ===\n";

    return 0;
}
