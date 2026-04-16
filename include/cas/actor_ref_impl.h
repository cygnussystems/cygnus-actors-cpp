#ifndef CAS_ACTOR_REF_IMPL_H
#define CAS_ACTOR_REF_IMPL_H

// actor_ref template implementations
// This file MUST be included AFTER actor.h is fully defined
// It provides the template definitions that depend on complete actor type

#include "actor_ref.h"
#include "actor.h"
#include "system.h"
#include "ask_message.h"

namespace cas {

// Template implementations for actor_ref methods that depend on complete actor type

template<typename MessageType>
void actor_ref::tell(const MessageType& msg) const {
    if (!m_actor_ptr) return;

    // Clone message from stack and enqueue
    auto msg_copy = std::make_unique<MessageType>(msg);

    // Set sender to current actor (thread-local context)
    // If called from within an actor's message handler, this will be that actor
    // If called from outside (e.g., main), sender will be invalid (nullptr actor_ptr)
    actor* current = actor::get_current_actor();
    if (current) {
        msg_copy->sender = current->self();
    }
    // else: sender remains default-constructed (invalid actor_ref)

    // Assign unique message ID
    msg_copy->message_id = system::next_message_id();

    // correlation_id remains as set by user (0 if not a reply)

    m_actor_ptr->enqueue_message(std::move(msg_copy));
}

template<typename MessageType>
void actor_ref::push(const MessageType& msg) const {
    tell(msg);  // Alias for receive
}

template<typename MessageType>
void actor_ref::enqueue(const MessageType& msg) const {
    tell(msg);  // Alias for receive
}

template<typename MessageType>
const actor_ref& actor_ref::operator<<(const MessageType& msg) const {
    tell(msg);
    return *this;
}

template<typename ReturnType, typename OpTag, typename... Args>
ReturnType actor_ref::ask(OpTag op, Args&&... args) {
    if (!m_actor_ptr) {
        throw std::runtime_error("Cannot ask on invalid actor_ref");
    }

    // Create ask request with promise/future
    auto request = std::make_unique<ask_request<ReturnType, OpTag, Args...>>(
        op, std::forward<Args>(args)...
    );

    // Get future before moving request
    std::future<ReturnType> future = request->promise.get_future();

    // Set sender
    actor* current = actor::get_current_actor();
    if (current) {
        request->sender = current->self();
    }

    // Assign message ID
    request->message_id = system::next_message_id();

    // Enqueue to ask queue (priority)
    m_actor_ptr->enqueue_ask_message(std::move(request));

    // Block waiting for result
    return future.get();
}

template<typename ReturnType, typename OpTag, typename... Args>
std::optional<ReturnType> actor_ref::ask(OpTag op, std::chrono::milliseconds timeout, Args&&... args) {
    if (!m_actor_ptr) {
        return std::nullopt;
    }

    // Create ask request with promise/future
    auto request = std::make_unique<ask_request<ReturnType, OpTag, Args...>>(
        op, std::forward<Args>(args)...
    );

    // Get future before moving request
    std::future<ReturnType> future = request->promise.get_future();

    // Set sender
    actor* current = actor::get_current_actor();
    if (current) {
        request->sender = current->self();
    }

    // Assign message ID
    request->message_id = system::next_message_id();

    // Enqueue to ask queue (priority)
    m_actor_ptr->enqueue_ask_message(std::move(request));

    // Wait with timeout
    if (future.wait_for(timeout) == std::future_status::ready) {
        return future.get();
    } else {
        return std::nullopt;  // Timeout
    }
}

} // namespace cas

#endif // CAS_ACTOR_REF_IMPL_H
