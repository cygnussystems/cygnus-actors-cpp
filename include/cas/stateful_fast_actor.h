#ifndef CAS_STATEFUL_FAST_ACTOR_H
#define CAS_STATEFUL_FAST_ACTOR_H

#include "fast_actor.h"
#include "detail/selective_stash.h"
#include <typeindex>

namespace cas {

/// stateful_actor on a dedicated thread: selective receive (accept some
/// message types per state, hold the rest in order) for an actor that also
/// needs fast_actor's latency - e.g. an exchange session that must not send
/// orders until logged in.
///
/// Same API and rules as stateful_actor; the filtering logic is shared
/// (detail::selective_stash).
class stateful_fast_actor : public fast_actor {
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
    stateful_fast_actor() = default;
    explicit stateful_fast_actor(polling_strategy strategy);
    ~stateful_fast_actor() override = default;

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

#endif // CAS_STATEFUL_FAST_ACTOR_H
