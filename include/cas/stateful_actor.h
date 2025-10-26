#ifndef CAS_STATEFUL_ACTOR_H
#define CAS_STATEFUL_ACTOR_H

#include "actor.h"
#include <set>
#include <typeindex>
#include <deque>

namespace cas {

/// Stateful actor with selective message processing
/// Messages not accepted in current state remain queued for later
class stateful_actor : public actor {
    friend class system;

private:
    // Override base mailbox with deque for selective extraction
    std::deque<std::unique_ptr<message_base>> m_stateful_mailbox;
    std::mutex m_stateful_mailbox_mutex;

    // Set of message types accepted in current state
    std::set<std::type_index> m_accepted_types;
    std::mutex m_accepted_types_mutex;

    // By default, accept all message types (same as base actor)
    bool m_accept_all = true;

protected:
    /// Accept only this message type in current state
    template<typename MessageType>
    void accept_message_type();

    /// Reject this message type in current state (leave queued)
    template<typename MessageType>
    void reject_message_type();

    /// Accept all message types (default behavior)
    void accept_all_message_types();

    /// Clear all accepted types (reject everything until explicitly accepted)
    void reject_all_message_types();

public:
    stateful_actor() = default;
    virtual ~stateful_actor() = default;

    // Override enqueue to use stateful mailbox
    void enqueue_message(std::unique_ptr<message_base> msg);

    // Override process to do selective extraction
    void process_next_message();

    // Override has_messages to check stateful mailbox
    bool has_messages();
};

// Template implementations

template<typename MessageType>
void stateful_actor::accept_message_type() {
    std::lock_guard<std::mutex> lock(m_accepted_types_mutex);
    m_accept_all = false;
    m_accepted_types.insert(typeid(MessageType));
}

template<typename MessageType>
void stateful_actor::reject_message_type() {
    std::lock_guard<std::mutex> lock(m_accepted_types_mutex);
    m_accepted_types.erase(typeid(MessageType));
}

} // namespace cas

#endif // CAS_STATEFUL_ACTOR_H