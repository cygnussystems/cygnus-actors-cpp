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
        if constexpr (ThreadSafe) {
            std::lock_guard<std::mutex> lock(m_handler_mutex);
            current_actor_context = this;
            dispatch_message(msg.get());
            current_actor_context = nullptr;
        } else {
            current_actor_context = this;
            dispatch_message(msg.get());
            current_actor_context = nullptr;
        }
    }

    // No ask queue needed - caller can directly access actor methods
    void enqueue_ask_message(std::unique_ptr<message_base> msg) override {
        // Process same as regular messages
        enqueue_message(std::move(msg));
    }

    // Inline actors don't need message processing loop
    void process_next_message() override {
        // No-op: messages processed synchronously
    }

    bool has_messages() override {
        return false;  // Never has queued messages
    }
};

} // namespace cas

#endif // CAS_INLINE_ACTOR_H
