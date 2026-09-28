#include "cas/stateful_actor.h"
#include "cas/message_base.h"

namespace cas {

void stateful_actor::accept_all_message_types() {
    m_stash.accept_all();
}

void stateful_actor::reject_all_message_types() {
    m_stash.reject_all();
}

void stateful_actor::process_next_message() {
    m_stash.process_next(*this);
}

bool stateful_actor::has_messages() {
    return m_stash.has_work(*this);
}

size_t stateful_actor::queue_size() const {
    return m_stash.queued(*this);
}

void stateful_actor::discard_pending_messages() {
    actor::discard_pending_messages();
    m_stash.drop_held(*this);
}

bool stateful_actor::service_stop_request() {
    const bool done = actor::service_stop_request();
    if (done && get_state() == actor_state::stopped) {
        // Messages still held when the stop completed are never handled.
        m_stash.drop_held(*this);
    }
    return done;
}

} // namespace cas
