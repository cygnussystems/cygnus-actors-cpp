// Standalone timer test - no actors involved
// This isolates the timer thread logic for debugging

#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <queue>
#include <atomic>
#include <functional>
#include <memory>

struct simple_timer {
    int id;
    std::chrono::steady_clock::time_point fire_time;
    std::function<void()> callback;

    bool operator>(const simple_timer& other) const {
        return fire_time > other.fire_time;  // Min heap
    }
};

class standalone_timer_manager {
private:
    std::priority_queue<std::shared_ptr<simple_timer>,
                       std::vector<std::shared_ptr<simple_timer>>,
                       std::greater<std::shared_ptr<simple_timer>>> timer_queue;
    std::mutex timer_mutex;
    std::condition_variable timer_cv;
    std::atomic<bool> running{false};
    std::thread timer_thread;
    int next_id = 1;

    void timer_thread_func() {
        std::cout << "[TIMER_THREAD] Started\n" << std::flush;

        while (running) {
            std::unique_lock<std::mutex> lock(timer_mutex);

            std::cout << "[TIMER_THREAD] Loop iteration, running=" << running.load()
                      << ", queue_size=" << timer_queue.size() << "\n" << std::flush;

            // Wait until we have a timer or shutdown
            timer_cv.wait(lock, [this]() {
                bool should_wake = !timer_queue.empty() || !running;
                std::cout << "[TIMER_THREAD] Wait predicate: queue_empty=" << timer_queue.empty()
                          << ", running=" << running.load() << ", should_wake=" << should_wake << "\n" << std::flush;
                return should_wake;
            });

            std::cout << "[TIMER_THREAD] Woke up, running=" << running.load() << "\n" << std::flush;

            // Check if we're shutting down
            if (!running) {
                std::cout << "[TIMER_THREAD] Shutdown detected, breaking\n" << std::flush;
                break;
            }

            // Get next timer
            auto next_timer = timer_queue.top();

            // Check if it's time to fire
            auto now = std::chrono::steady_clock::now();
            if (now >= next_timer->fire_time) {
                std::cout << "[TIMER_THREAD] Firing timer " << next_timer->id << "\n" << std::flush;
                timer_queue.pop();

                // Fire callback (outside lock to avoid deadlock)
                lock.unlock();
                next_timer->callback();
                lock.lock();
            } else {
                // Not ready yet - wait until fire time
                auto wait_duration = next_timer->fire_time - now;
                std::cout << "[TIMER_THREAD] Waiting "
                          << std::chrono::duration_cast<std::chrono::milliseconds>(wait_duration).count()
                          << "ms for timer " << next_timer->id << "\n" << std::flush;
                timer_cv.wait_for(lock, wait_duration);
            }
        }

        std::cout << "[TIMER_THREAD] Exiting\n" << std::flush;
    }

public:
    void start() {
        std::cout << "[MAIN] Starting timer manager\n" << std::flush;
        running = true;
        timer_thread = std::thread(&standalone_timer_manager::timer_thread_func, this);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));  // Let thread start
        std::cout << "[MAIN] Timer manager started\n" << std::flush;
    }

    void stop() {
        std::cout << "[MAIN] Stopping timer manager\n" << std::flush;
        {
            std::lock_guard<std::mutex> lock(timer_mutex);
            running = false;
        }
        timer_cv.notify_all();

        std::cout << "[MAIN] Waiting for timer thread to join\n" << std::flush;
        if (timer_thread.joinable()) {
            timer_thread.join();
        }
        std::cout << "[MAIN] Timer manager stopped\n" << std::flush;
    }

    int schedule(std::chrono::milliseconds delay, std::function<void()> callback) {
        int id = next_id++;
        auto fire_time = std::chrono::steady_clock::now() + delay;
        auto timer = std::make_shared<simple_timer>();
        timer->id = id;
        timer->fire_time = fire_time;
        timer->callback = std::move(callback);

        std::cout << "[MAIN] Scheduling timer " << id << " with delay " << delay.count() << "ms\n" << std::flush;

        {
            std::lock_guard<std::mutex> lock(timer_mutex);
            timer_queue.push(timer);
        }
        timer_cv.notify_one();

        return id;
    }
};

int main() {
    std::cout << "=== Standalone Timer Test ===\n" << std::flush;

    std::atomic<int> fire_count{0};

    standalone_timer_manager mgr;
    mgr.start();

    // Schedule a simple timer
    std::cout << "\n[TEST] Scheduling timer for 100ms\n" << std::flush;
    mgr.schedule(std::chrono::milliseconds(100), [&fire_count]() {
        std::cout << "[CALLBACK] Timer fired!\n" << std::flush;
        fire_count++;
    });

    // Wait for it to fire
    std::cout << "[TEST] Waiting 200ms for timer to fire\n" << std::flush;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::cout << "[TEST] Fire count: " << fire_count.load() << " (expected: 1)\n" << std::flush;

    // Stop
    std::cout << "\n[TEST] Stopping manager\n" << std::flush;
    mgr.stop();

    std::cout << "\n=== Test Complete ===\n" << std::flush;
    std::cout << "Result: " << (fire_count.load() == 1 ? "PASS" : "FAIL") << "\n" << std::flush;

    return (fire_count.load() == 1) ? 0 : 1;
}
