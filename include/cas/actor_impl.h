#ifndef CAS_ACTOR_IMPL_H
#define CAS_ACTOR_IMPL_H

// Template implementations for actor methods that depend on system
// This file is included by cas.h AFTER system.h is fully defined

#include "actor.h"
#include "system.h"

namespace cas {

// Template implementations for timer methods

template<typename MessageType>
timer_id actor::schedule_once(std::chrono::milliseconds delay, MessageType msg) {
    // Verify message is copy-constructible at compile time
    static_assert(std::is_copy_constructible_v<MessageType>,
                  "Timer messages must be copy-constructible");

    // Clone the message to ensure proper ownership
    auto msg_ptr = std::make_unique<MessageType>(msg);

    // Create copy function (captures a copy of the message)
    auto copier = [msg]() -> std::unique_ptr<message_base> {
        return std::make_unique<MessageType>(msg);
    };

    // Schedule with system
    timer_id id = system::schedule_timer(this, std::move(msg_ptr), std::move(copier), delay);

    // Track this timer
    m_active_timers.insert(id);

    return id;
}

template<typename MessageType>
timer_id actor::schedule_periodic(std::chrono::milliseconds interval, MessageType msg) {
    // Verify message is copy-constructible at compile time
    static_assert(std::is_copy_constructible_v<MessageType>,
                  "Timer messages must be copy-constructible");

    // Clone the message to ensure proper ownership
    auto msg_ptr = std::make_unique<MessageType>(msg);

    // Create copy function (captures a copy of the message)
    auto copier = [msg]() -> std::unique_ptr<message_base> {
        return std::make_unique<MessageType>(msg);
    };

    // Schedule with system (interval parameter makes it periodic)
    timer_id id = system::schedule_timer(this, std::move(msg_ptr), std::move(copier), interval, interval);

    // Track this timer
    m_active_timers.insert(id);

    return id;
}

} // namespace cas

#endif // CAS_ACTOR_IMPL_H
