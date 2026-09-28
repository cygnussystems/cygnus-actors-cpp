#ifndef CAS_INLINE_ACTOR_H
#define CAS_INLINE_ACTOR_H

#include "actor.h"
#include <mutex>
#include <variant>
#include <deque>
#include <thread>
#include <atomic>
#include "call_message.h"

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

    // Thread currently running one of this actor's handlers (default id when
    // none). A send from that same thread - the handler telling itself, or a
    // chain of inline actors leading back here - must not dispatch nested
    // (the mutex is not recursive, and handlers never nest), so it is queued
    // in m_deferred and handled once the current handler returns.
    std::atomic<std::thread::id> m_dispatching_thread{};

    // Touched only by the thread in m_dispatching_thread
    std::deque<std::unique_ptr<message_base>> m_deferred;

public:
    inline_actor() = default;
    virtual ~inline_actor() = default;

    bool runs_inline() const override {
        return true;
    }

    // Override to process messages synchronously in caller's thread
    void enqueue_message(std::unique_ptr<message_base> msg) override {
        if (is_reentrant()) {
            // Accepted like a queued message: handled after the current
            // handler, even if that handler started a stop (as a pooled actor
            // drains). Rejected once the actor is no longer running.
            if (get_state() != actor_state::running) {
                report_rejected(msg.get(), false);
                return;
            }
            m_deferred.push_back(std::move(msg));
            return;
        }
        dispatch_inline(std::move(msg), false);
    }

    // No queueing - the call is processed before this returns
    void enqueue_call_message(std::unique_ptr<message_base> msg) override {
        if (is_reentrant()) {
            // The caller blocks until the call is handled, but it can only be
            // handled after the handler the caller is running in returns.
            throw call_deadlock_error(
                "call() would deadlock: inline actor '" + name() +
                "' is already handling a message on this thread.");
        }
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
    bool is_reentrant() const {
        return m_dispatching_thread.load(std::memory_order_relaxed) == std::this_thread::get_id();
    }

    void dispatch_inline(std::unique_ptr<message_base> msg, bool was_call) {
        // Checked before locking so a handler or hook that sends to this
        // actor while it is stopping is dropped rather than self-deadlocking
        // on the (non-recursive) mutex held by the stop.
        if (get_state() != actor_state::running) {
            report_rejected(msg.get(), was_call);
            return;
        }

        if constexpr (ThreadSafe) {
            std::lock_guard<std::mutex> lock(m_handler_mutex);
            // Re-check under the lock: a stop may have completed while we waited
            if (get_state() != actor_state::running) {
                report_rejected(msg.get(), was_call);
                return;
            }
            run_turn(msg.get());
        } else {
            run_turn(msg.get());
        }
    }

    // Handles msg, then whatever its handler (transitively) sent back to this
    // actor on this thread, in send order.
    void run_turn(message_base* msg) {
        struct turn_scope {
            inline_actor& self;
            actor* prev_context = current_actor_context;

            explicit turn_scope(inline_actor& a) : self(a) {
                current_actor_context = &self;
                self.m_dispatching_thread.store(std::this_thread::get_id(), std::memory_order_relaxed);
            }
            ~turn_scope() {
                // Also on a handler exception (which propagates to the sender):
                // messages deferred behind it are never handled.
                for (auto& m : self.m_deferred) {
                    self.report_rejected(m.get(), false);
                }
                self.m_deferred.clear();
                self.m_dispatching_thread.store(std::thread::id{}, std::memory_order_relaxed);
                current_actor_context = prev_context;
            }
        } scope(*this);

        dispatch_message(msg);
        while (!m_deferred.empty()) {
            std::unique_ptr<message_base> next = std::move(m_deferred.front());
            m_deferred.pop_front();
            dispatch_message(next.get());
        }
    }
};

} // namespace cas

#endif // CAS_INLINE_ACTOR_H
