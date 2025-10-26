#ifndef CAS_TIMER_H
#define CAS_TIMER_H

#include <cstdint>
#include <chrono>
#include <memory>
#include <functional>
#include "message_base.h"

namespace cas {

// Forward declarations
class actor;

// Unique identifier for timers
using timer_id = uint64_t;
constexpr timer_id INVALID_TIMER_ID = 0;

// Internal timer representation
struct scheduled_timer {
    timer_id id;
    actor* target_actor;
    std::unique_ptr<message_base> message;
    std::function<std::unique_ptr<message_base>()> copy_message;  // Function to copy the message
    std::chrono::steady_clock::time_point next_fire_time;
    std::chrono::milliseconds interval;  // 0 for one-shot, >0 for periodic
    bool cancelled;

    scheduled_timer(timer_id tid, actor* target, std::unique_ptr<message_base> msg,
                   std::function<std::unique_ptr<message_base>()> copier,
                   std::chrono::steady_clock::time_point fire_time,
                   std::chrono::milliseconds repeat_interval = std::chrono::milliseconds(0))
        : id(tid)
        , target_actor(target)
        , message(std::move(msg))
        , copy_message(std::move(copier))
        , next_fire_time(fire_time)
        , interval(repeat_interval)
        , cancelled(false)
    {}

    // For priority queue ordering (earlier times have higher priority)
    bool operator>(const scheduled_timer& other) const {
        return next_fire_time > other.next_fire_time;
    }
};

} // namespace cas

#endif // CAS_TIMER_H
