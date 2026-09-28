#ifndef CAS_STATEFUL_ACTOR_H
#define CAS_STATEFUL_ACTOR_H

#include "actor.h"
#include "detail/selective_stash.h"
#include <typeindex>

namespace cas {

/// Pooled actor with selective receive: in each state it accepts only some
/// message types; the others stay queued (in order) until a later state
/// accepts them.
///
/// Calls (actor_ref::call) and system shutdown are never held back.
/// Accept/reject from this actor's own on_start() or handlers only.
/// For the same on a dedicated thread, see stateful_fast_actor.
class stateful_actor : public actor {
protected:
    /// Accept this message type in the current state (turns filtering on)
    template<typename MessageType>
    void accept_message_type() { m_stash.accept(typeid(MessageType)); }

    /// Stop accepting this message type; its messages wait until accepted
    template<typename MessageType>
    void reject_message_type() { m_stash.reject(typeid(MessageType)); }

    /// Accept all message types (default behavior)
    void accept_all_message_types();

    /// Accept nothing until types are accepted explicitly
    void reject_all_message_types();

public:
    stateful_actor() = default;
    ~stateful_actor() override = default;

    // Internal: selective extraction (see detail::selective_stash)
    void process_next_message() override;
    bool has_messages() override;
    size_t queue_size() const override;
    void discard_pending_messages() override;
    bool service_stop_request() override;

private:
    detail::selective_stash m_stash;
};

} // namespace cas

#endif // CAS_STATEFUL_ACTOR_H
