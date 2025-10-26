#include "cas/timer_manager.h"

namespace cas {

timer_manager::timer_manager() = default;

timer_manager::~timer_manager() {
    if (m_running) {
        stop();
    }
}

void timer_manager::start() {
    if (m_running) {
        return;  // Already running
    }

    m_running = true;
    m_timer_thread = std::thread(&timer_manager::timer_thread_func, this);
}

void timer_manager::stop() {
    // Set running to false while holding mutex, then notify
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_running = false;
        m_cv.notify_all();
    }

    // Wait for thread to finish
    if (m_timer_thread.joinable()) {
        m_timer_thread.join();
    }

    // Clear all timers
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        while (!m_timer_queue.empty()) {
            m_timer_queue.pop();
        }
        m_timer_lookup.clear();
        m_callbacks.clear();
    }
}

bool timer_manager::is_running() const {
    return m_running.load();
}

timer_id timer_manager::schedule(
    std::unique_ptr<message_base> msg,
    std::function<std::unique_ptr<message_base>()> copy_func,
    std::chrono::milliseconds delay,
    std::chrono::milliseconds interval,
    timer_callback callback
) {
    // Calculate fire time
    auto fire_time = std::chrono::steady_clock::now() + delay;

    // Generate unique ID
    timer_id id = m_next_timer_id.fetch_add(1);

    // Create timer (note: target_actor is nullptr since we use callbacks)
    auto timer = std::make_shared<scheduled_timer>(
        id,
        nullptr,  // No target actor
        std::move(msg),
        std::move(copy_func),
        fire_time,
        interval
    );

    // Add to queue and lookup map
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_timer_queue.push(timer);
        m_timer_lookup[id] = timer;
        m_callbacks[id] = std::move(callback);
    }

    // Wake up timer thread
    m_cv.notify_one();

    return id;
}

void timer_manager::cancel(timer_id id) {
    std::lock_guard<std::mutex> lock(m_mutex);

    // Find timer in lookup map
    auto it = m_timer_lookup.find(id);
    if (it != m_timer_lookup.end()) {
        // Mark as cancelled (timer thread will skip it)
        it->second->cancelled = true;
        // Remove from lookup map
        m_timer_lookup.erase(it);
    }

    // Also remove callback
    m_callbacks.erase(id);
}

size_t timer_manager::active_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_timer_lookup.size();
}

void timer_manager::timer_thread_func() {
    while (m_running.load()) {
        std::unique_lock<std::mutex> lock(m_mutex);

        // Wait until we have a timer or shutdown
        m_cv.wait(lock, [this]() {
            return !m_timer_queue.empty() || !m_running.load();
        });

        // Check if we're shutting down
        if (!m_running.load()) {
            break;
        }

        // Get next timer (don't pop yet)
        auto next_timer = m_timer_queue.top();

        // Check if it's time to fire
        auto now = std::chrono::steady_clock::now();
        if (now >= next_timer->next_fire_time) {
            // Pop from queue
            m_timer_queue.pop();

            // Skip if cancelled
            if (!next_timer->cancelled) {
                timer_id id = next_timer->id;

                // Get callback
                auto callback_it = m_callbacks.find(id);
                if (callback_it != m_callbacks.end()) {
                    timer_callback callback = callback_it->second;

                    // For periodic timers, reschedule
                    if (next_timer->interval.count() > 0) {
                        // Copy the message for this firing
                        auto msg_copy = next_timer->copy_message();

                        // Update fire time for next occurrence
                        next_timer->next_fire_time += next_timer->interval;

                        // Push back into queue for next firing
                        m_timer_queue.push(next_timer);

                        // Fire callback (release lock first to avoid deadlock)
                        lock.unlock();
                        callback(id, std::move(msg_copy));
                        lock.lock();
                    } else {
                        // One-shot timer - move the message
                        auto msg = std::move(next_timer->message);

                        // Remove from lookup and callbacks
                        m_timer_lookup.erase(id);
                        m_callbacks.erase(id);

                        // Fire callback (release lock first to avoid deadlock)
                        lock.unlock();
                        callback(id, std::move(msg));
                        lock.lock();
                    }
                }
            }
        } else {
            // Not ready yet - wait until fire time
            auto wait_duration = next_timer->next_fire_time - now;
            m_cv.wait_for(lock, wait_duration);
        }
    }
}

} // namespace cas
