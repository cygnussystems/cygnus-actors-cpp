// Latency benchmark: round-trip time between two actors.
//
// Two actors play ping-pong with one message in flight at a time. The pinger
// stamps each ping, the ponger replies to msg.sender, and the pinger records
// the round trip when the pong arrives before sending the next ping. Each
// sample therefore includes both handlers, two mailbox hand-offs and any
// worker wake-ups - the "an event arrives: how soon is it handled?" path.
//
// Reported as percentiles, not an average: for latency-sensitive work the
// tail (p99, p99.9, max) matters more than the typical case, and an average
// hides it. Times are round trips; one-way latency is roughly half.
//
// Clock: std::chrono::steady_clock. On Windows that is QueryPerformanceCounter,
// typically 100 ns resolution, so sub-microsecond figures are approximate.

#include "cas/cas.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

using clock_type = std::chrono::steady_clock;

struct ping : cas::message_base {
    clock_type::time_point sent;
};

struct pong : cas::message_base {
    clock_type::time_point sent;  // echoed back from the ping
};

struct start_run : cas::message_base {
    cas::actor_ref ponger;
};

constexpr int k_warmup = 5'000;
constexpr int k_samples = 50'000;

// Written by the pinger, read by main() once g_done is set (release/acquire).
std::vector<int64_t> g_round_trips_ns;
std::atomic<bool> g_done{false};

// Base is cas::actor (pooled) or cas::fast_actor (dedicated thread).
template<typename Base>
class ponger : public Base {
protected:
    void on_start() override {
        this->template handler<ping>(&ponger::on_ping);
    }

    void on_ping(const ping& msg) {
        pong reply;
        reply.sent = msg.sent;
        msg.sender.tell(reply);
    }
};

template<typename Base>
class pinger : public Base {
protected:
    void on_start() override {
        this->template handler<start_run>(&pinger::on_start_run);
        this->template handler<pong>(&pinger::on_pong);
    }

    void on_start_run(const start_run& msg) {
        m_ponger = msg.ponger;
        m_count = 0;
        send_ping();
    }

    void on_pong(const pong& msg) {
        const auto rtt = clock_type::now() - msg.sent;
        if (m_count >= k_warmup) {
            g_round_trips_ns.push_back(
                std::chrono::duration_cast<std::chrono::nanoseconds>(rtt).count());
        }
        if (++m_count < k_warmup + k_samples) {
            send_ping();
        } else {
            g_done.store(true, std::memory_order_release);
        }
    }

private:
    void send_ping() {
        ping p;
        p.sent = clock_type::now();
        m_ponger.tell(p);
    }

    cas::actor_ref m_ponger;
    int m_count = 0;
};

int64_t percentile(const std::vector<int64_t>& sorted, double p) {
    const size_t idx = static_cast<size_t>(p / 100.0 * static_cast<double>(sorted.size() - 1));
    return sorted[idx];
}

template<typename PingerBase, typename PongerBase>
void run_scenario(const std::string& name) {
    cas::system::reset();
    g_round_trips_ns.clear();
    g_round_trips_ns.reserve(k_samples);
    g_done = false;

    auto p = cas::system::create<pinger<PingerBase>>();
    auto q = cas::system::create<ponger<PongerBase>>();
    cas::system::start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    start_run go;
    go.ponger = q;
    p.tell(go);

    const auto deadline = clock_type::now() + std::chrono::seconds(60);
    while (!g_done.load(std::memory_order_acquire) && clock_type::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const bool completed = g_done.load(std::memory_order_acquire);

    cas::system::shutdown();
    cas::system::wait_for_shutdown();
    cas::system::reset();

    if (!completed) {
        std::cout << name << ": TIMEOUT after " << g_round_trips_ns.size() << " samples\n";
        return;
    }

    std::vector<int64_t> sorted = g_round_trips_ns;
    std::sort(sorted.begin(), sorted.end());
    auto us = [](int64_t ns) { return static_cast<double>(ns) / 1000.0; };

    std::cout << std::fixed << std::setprecision(1);
    std::cout << name << " (round trip, us, " << sorted.size() << " samples):\n"
              << "  min " << us(sorted.front())
              << "  p50 " << us(percentile(sorted, 50))
              << "  p90 " << us(percentile(sorted, 90))
              << "  p99 " << us(percentile(sorted, 99))
              << "  p99.9 " << us(percentile(sorted, 99.9))
              << "  max " << us(sorted.back()) << "\n\n"
              << std::flush;
}

} // namespace

int main() {
    std::cout << "=== Cygnus Actor Framework Latency Benchmark ===\n\n" << std::flush;

    run_scenario<cas::actor, cas::actor>("pooled -> pooled");
    run_scenario<cas::fast_actor, cas::fast_actor>("fast -> fast");
    run_scenario<cas::actor, cas::fast_actor>("pooled -> fast");

    std::cout << "=== Benchmark Complete ===\n";
    return 0;
}
