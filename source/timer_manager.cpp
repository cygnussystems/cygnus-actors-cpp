#include "cas/timer_manager.h"

namespace cas {

timer_manager::timer_manager() = default;

// Joins the worker before any member it uses is destroyed.
//
// jthread does request stop and join on its own, but relying on that alone is
// not enough here: it only happens once ~timer_manager()'s body has run and
// member destruction reaches m_timer_thread. Calling stop() here joins the
// worker while the mutex, condition variable and containers are guaranteed
// alive, instead of depending solely on member declaration order.
timer_manager::~timer_manager() {
    stop();
}

void timer_manager::start() {
    if (m_timer_thread.joinable()) {
        return;  // Already running
    }

    m_running.store(true, std::memory_order_release);

    m_timer_thread = std::jthread([this](std::stop_token stop) {
        timer_thread_func(std::move(stop));
    });
}

void timer_manager::stop() {
    // Publish "not running" before the join, matching the old atomic-flag
    // timing: observers see false as soon as shutdown begins rather than only
    // once the worker has fully exited.
    m_running.store(false, std::memory_order_release);

    // request_stop() interrupts any condition_variable_any wait registered
    // with the token, so no separate notify is needed to wake the thread.
    m_timer_thread.request_stop();

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
    // Reads the atomic rather than m_timer_thread: see m_running's declaration
    // for why inspecting the jthread here would race start()/stop().
    return m_running.load(std::memory_order_acquire);
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
    {
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

    // Wake the timer thread. A cancelled timer stays at the head of the queue
    // with its original fire time, so without this the thread sleeps until
    // that time before noticing - and if the cancelled timer was the only one,
    // a caller waiting on active_count() to drop sees no progress until then.
    m_cv.notify_one();
}

size_t timer_manager::active_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_timer_lookup.size();
}

void timer_manager::timer_thread_func(std::stop_token stop) {
    std::unique_lock<std::mutex> lock(m_mutex);

    while (!stop.stop_requested()) {
        // Wait for a timer to exist. The stop_token overload returns false
        // only when stop was requested, so a stop during the wait exits the
        // loop immediately rather than waiting for a notify.
        if (!m_cv.wait(lock, stop, [this]() { return !m_timer_queue.empty(); })) {
            break;
        }

        auto next_timer = m_timer_queue.top();

        // Drop cancelled timers as soon as they reach the head, without
        // waiting for their fire time. Cancellation leaves the timer in the
        // queue (a priority_queue cannot remove from the middle), so this is
        // where it actually leaves.
        if (next_timer->cancelled) {
            m_timer_queue.pop();
            continue;
        }

        auto now = std::chrono::steady_clock::now();

        if (now < next_timer->next_fire_time) {
            // Not ready yet. Wait until its fire time, but wake early if a
            // sooner timer is scheduled or stop is requested.
            //
            // The predicate re-checks the queue head rather than assuming it
            // is still next_timer: schedule() may insert an earlier timer
            // while this wait is in progress. Previously this was a bare
            // wait_for whose result was discarded, so neither an earlier
            // timer nor a shutdown was observed until the full duration had
            // elapsed.
            const auto deadline = next_timer->next_fire_time;
            m_cv.wait_until(lock, stop, deadline, [this, deadline]() {
                if (m_timer_queue.empty()) {
                    return true;
                }
                const auto& head = m_timer_queue.top();
                // A cancelled head must wake the wait: it keeps its original
                // fire time, so waiting for that time would stall until a
                // deadline that no longer needs to be honoured.
                return head->cancelled || head->next_fire_time < deadline;
            });
            continue;  // Re-evaluate from the top: the head may have changed
        }

        // Fire time reached - take it off the queue
        m_timer_queue.pop();

        if (next_timer->cancelled) {
            continue;
        }

        const timer_id id = next_timer->id;

        auto callback_it = m_callbacks.find(id);
        if (callback_it == m_callbacks.end()) {
            continue;
        }
        timer_callback callback = callback_it->second;

        const bool is_periodic = next_timer->interval.count() > 0;
        std::unique_ptr<message_base> msg;

        if (is_periodic) {
            msg = next_timer->copy_message();
            next_timer->next_fire_time += next_timer->interval;
            m_timer_queue.push(next_timer);
        } else {
            msg = std::move(next_timer->message);
            m_timer_lookup.erase(id);
            m_callbacks.erase(id);
        }

        // Fire outside the lock: the callback enqueues to an actor and must
        // not run with m_mutex held. Everything this iteration needs was
        // copied out above, so nothing reads shared state after the unlock -
        // the queue head in particular is re-read from the top of the loop
        // rather than assumed to be unchanged.
        lock.unlock();
        callback(id, std::move(msg));
        lock.lock();
    }
}

} // namespace cas
