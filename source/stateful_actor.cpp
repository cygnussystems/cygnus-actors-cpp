#include "cas/stateful_actor.h"
#include "cas/message_base.h"

namespace cas {

void stateful_actor::accept_all_message_types() {
    std::lock_guard<std::mutex> lock(m_accepted_types_mutex);
    m_accept_all = true;
    m_accepted_types.clear();
}

void stateful_actor::reject_all_message_types() {
    std::lock_guard<std::mutex> lock(m_accepted_types_mutex);
    m_accept_all = false;
    m_accepted_types.clear();
}

void stateful_actor::take_from_mailbox() {
    std::lock_guard<std::mutex> lock(m_stateful_mailbox_mutex);
    while (std::unique_ptr<message_base> msg = m_mailbox.pop()) {
        m_stateful_mailbox.push_back(std::move(msg));
    }
}

void stateful_actor::process_next_message() {
    take_from_mailbox();
    std::unique_ptr<message_base> msg;

    // Find first message with accepted type
    {
        std::lock_guard<std::mutex> mailbox_lock(m_stateful_mailbox_mutex);
        std::lock_guard<std::mutex> types_lock(m_accepted_types_mutex);

        if (m_stateful_mailbox.empty()) {
            return;
        }

        // If accepting all types, just take first message
        if (m_accept_all) {
            msg = std::move(m_stateful_mailbox.front());
            m_stateful_mailbox.pop_front();
        } else {
            // Scan for first message with accepted type
            for (auto it = m_stateful_mailbox.begin(); it != m_stateful_mailbox.end(); ++it) {
                if (m_accepted_types.find(typeid(**it)) != m_accepted_types.end()) {
                    msg = std::move(*it);
                    m_stateful_mailbox.erase(it);
                    break;
                }
            }
            // If no acceptable message found, return without processing
            if (!msg) {
                return;
            }
        }
    }

    // Dispatch message (same as base actor)
    if (msg) {
        actor* previous = current_actor_context;
        current_actor_context = this;
        dispatch_message(msg.get());
        current_actor_context = previous;
    }
}

bool stateful_actor::has_messages() {
    // Anything not yet moved out of the base mailbox may be acceptable.
    if (m_mailbox.size() > 0) {
        return true;
    }
    std::lock_guard<std::mutex> mailbox_lock(m_stateful_mailbox_mutex);
    std::lock_guard<std::mutex> types_lock(m_accepted_types_mutex);

    if (m_stateful_mailbox.empty()) {
        return false;
    }

    // If accepting all types, any message counts
    if (m_accept_all) {
        return true;
    }

    // Check if any message has accepted type
    for (const auto& msg : m_stateful_mailbox) {
        if (m_accepted_types.find(typeid(*msg)) != m_accepted_types.end()) {
            return true;
        }
    }

    return false;
}

} // namespace cas
