#ifndef CAS_FAST_ACTOR_H
#define CAS_FAST_ACTOR_H

#include "actor.h"
#include <thread>
#include <chrono>

namespace cas {

/// Polling strategy for fast actors when no messages available
enum class polling_strategy {
    /// Yield to scheduler (default) - Low latency (~1-10μs), cooperative CPU usage
    yield,

    /// Spin briefly then yield - Ultra-low latency (<1μs), moderate CPU usage
    hybrid,

    /// Continuous busy-wait - Minimum latency (<100ns), 100% CPU usage
    busy_wait
};

/// Fast actor with dedicated thread and configurable polling strategy
/// Each fast_actor runs on its own thread with tight polling loop
/// Use for high-frequency trading, game loops, real-time control
/// Note: Uses more system resources than pooled actors
class fast_actor : public actor {
    friend class system;

private:
    // Each fast actor gets its own dedicated thread
    std::thread m_dedicated_thread;

    // Polling strategy when no messages available
    polling_strategy m_polling_strategy{polling_strategy::yield};

    // Spin count for hybrid strategy
    int m_spin_count{100};

    // Tight polling loop for this actor
    void run_dedicated_thread();

    // Cross-platform CPU pause instruction
    inline void cpu_pause();

protected:
    /// Set the polling strategy (default: yield)
    /// Call in constructor or on_start()
    void set_polling_strategy(polling_strategy strategy);

    /// Set spin count for hybrid strategy (default: 100)
    /// Only used when strategy is hybrid
    void set_spin_count(int count);

public:
    fast_actor() = default;
    explicit fast_actor(polling_strategy strategy);
    virtual ~fast_actor() = default;

    // Fast actors always run on dedicated threads
    bool is_fast_actor() const { return true; }
};

} // namespace cas

#endif // CAS_FAST_ACTOR_H
