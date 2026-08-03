#ifndef CAS_TIMER_MANAGER_H
#define CAS_TIMER_MANAGER_H

#include <thread>
#include <stop_token>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <unordered_map>
#include <atomic>
#include <functional>
#include <memory>
#include "timer.h"

namespace cas {

/// Manages timer scheduling and execution
/// Runs a dedicated thread that fires timers at the correct times
/// Thread-safe and can be used independently of the actor system
class timer_manager {
public:
    /// Callback type for when a timer fires
    /// Parameters: timer_id, message to deliver
    using timer_callback = std::function<void(timer_id, std::unique_ptr<message_base>)>;

    timer_manager();
    ~timer_manager();

    // Non-copyable, non-movable
    timer_manager(const timer_manager&) = delete;
    timer_manager& operator=(const timer_manager&) = delete;
    timer_manager(timer_manager&&) = delete;
    timer_manager& operator=(timer_manager&&) = delete;

    /// Start the timer thread
    void start();

    /// Stop the timer thread (waits for it to finish)
    void stop();

    /// Check if the manager is running
    bool is_running() const;

    /// Schedule a new timer
    /// @param msg Message to deliver when timer fires (ownership transferred)
    /// @param copy_func Function to copy the message (for periodic timers)
    /// @param delay How long until first firing
    /// @param interval Repeat interval (0 for one-shot)
    /// @param callback Function to call when timer fires
    /// @return Unique timer ID
    timer_id schedule(
        std::unique_ptr<message_base> msg,
        std::function<std::unique_ptr<message_base>()> copy_func,
        std::chrono::milliseconds delay,
        std::chrono::milliseconds interval,
        timer_callback callback
    );

    /// Cancel a timer by ID
    /// Safe to call multiple times with the same ID
    /// Safe to call with invalid IDs
    void cancel(timer_id id);

    /// Get count of active (non-cancelled) timers
    size_t active_count() const;

private:
    /// Timer thread main loop. Runs until the stop token is signalled.
    void timer_thread_func(std::stop_token stop);

    /// Comparator for priority queue (min-heap by fire time)
    struct timer_comparator {
        bool operator()(const std::shared_ptr<scheduled_timer>& a,
                       const std::shared_ptr<scheduled_timer>& b) const {
            return *a > *b;  // Reversed for min-heap
        }
    };

    // Synchronization.
    // condition_variable_any (not condition_variable) so waits can take the
    // stop_token and be interrupted the moment stop is requested.
    mutable std::mutex m_mutex;
    std::condition_variable_any m_cv;

    // Timer storage
    std::priority_queue<
        std::shared_ptr<scheduled_timer>,
        std::vector<std::shared_ptr<scheduled_timer>>,
        timer_comparator
    > m_timer_queue;

    std::unordered_map<timer_id, std::shared_ptr<scheduled_timer>> m_timer_lookup;

    // ID generation
    std::atomic<timer_id> m_next_timer_id{1};

    // Lifecycle observation.
    //
    // is_running() must not inspect m_timer_thread: start() assigns to that
    // object and stop() joins it, and concurrent mutation/inspection of the
    // jthread itself is a data race even though its stop state is internally
    // thread-safe. This flag gives observers a race-free read, mirroring the
    // atomic the pre-C++20 implementation had. The stop_token remains the
    // mechanism that actually stops the worker.
    std::atomic<bool> m_running{false};

    // Callback storage (one callback per timer, indexed by timer_id)
    std::unordered_map<timer_id, timer_callback> m_callbacks;

    // Thread. jthread owns the stop state and joins in its own destructor.
    //
    // DECLARATION ORDER IS LOAD-BEARING: this must stay the LAST member.
    // Members are destroyed in reverse declaration order, so declaring it last
    // means it is destroyed FIRST - requesting stop and joining the worker
    // while the mutex, condition variable, queue and maps it touches are all
    // still alive. Move it above them and ~timer_manager() tears down the
    // worker's state out from under a running thread.
    //
    // ~timer_manager() also calls stop() explicitly, so correctness does not
    // rest on this ordering alone; the two together are deliberate.
    std::jthread m_timer_thread;
};

} // namespace cas

#endif // CAS_TIMER_MANAGER_H
