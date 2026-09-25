#ifndef CAS_INLINE_ACTOR_H
#define CAS_INLINE_ACTOR_H

#include "actor.h"
#include <mutex>
#include <variant>

namespace cas {

/// Inline actor with synchronous message processing
/// Messages are processed immediately in the sender's thread (no queuing)
/// Template parameter controls thread safety:
///   ThreadSafe=true:  Protected by mutex, safe for multiple senders
///   ThreadSafe=false: No mutex, must only be called by single sender
template<bool ThreadSafe = true>
class inline_actor : public actor {
private:
    // Mutex only instantiated when ThreadSafe=true
    typename std::conditional<ThreadSafe, std::mutex, std::monostate>::type m_handler_mutex;

public:
    inline_actor() = default;
    virtual ~inline_actor() = default;

    // Override to process messages synchronously in caller's thread
    void enqueue_message(std::unique_ptr<message_base> msg) override {
        dispatch_inline(std::move(msg), false);
    }

    // No ask queue needed - the request is processed before this returns
    void enqueue_ask_message(std::unique_ptr<message_base> msg) override {
        dispatch_inline(std::move(msg), true);
    }

    // A stop is serialised with synchronous dispatch by the same mutex, so no
    // handler can be mid-flight during on_shutdown()/on_stop(), and none runs
    // after (dispatch_inline re-checks the state under the lock).
    bool service_stop_request() override {
        if constexpr (ThreadSafe) {
            std::lock_guard<std::mutex> lock(m_handler_mutex);
            return actor::service_stop_request();
        } else {
            return actor::service_stop_request();
        }
    }

    // Inline actors don't need message processing loop
    void process_next_message() override {
        // No-op: messages processed synchronously
    }

    bool has_messages() override {
        return false;  // Never has queued messages
    }

private:
    void dispatch_inline(std::unique_ptr<message_base> msg, bool was_ask) {
        // Checked before locking so a handler or hook that sends to this
        // actor while it is stopping is dropped rather than self-deadlocking
        // on the (non-recursive) mutex held by the stop.
        if (get_state() != actor_state::running) {
            report_rejected(msg.get(), was_ask);
            return;
        }

        if constexpr (ThreadSafe) {
            std::lock_guard<std::mutex> lock(m_handler_mutex);
            // Re-check under the lock: a stop may have completed while we waited
            if (get_state() != actor_state::running) {
                report_rejected(msg.get(), was_ask);
                return;
            }
            run_handler(msg.get());
        } else {
            run_handler(msg.get());
        }
    }

    void run_handler(message_base* msg) {
        actor* prev_context = current_actor_context;
        current_actor_context = this;
        dispatch_message(msg);
        current_actor_context = prev_context;
    }
};

} // namespace cas

#endif // CAS_INLINE_ACTOR_H
