#ifndef CAS_MESSAGE_BASE_H
#define CAS_MESSAGE_BASE_H

#include "actor_ref.h"
#include <cstdint>

namespace cas {

// Base struct for all messages
// Messages inherit from this to get sender information and correlation tracking
struct message_base {
    actor_ref sender;           // Who sent this message (set by framework)
    uint64_t message_id = 0;    // Unique ID for this message (set by framework)
    uint64_t correlation_id = 0; // ID of message this is replying to (0 = not a reply)
    virtual ~message_base() = default;  // Make polymorphic to preserve type info
};

// System message sent during shutdown
// Actors receive this and should call on_shutdown() and stop accepting external messages
struct system_shutdown : public message_base {};

// System message sent when an actor's queue exceeds threshold
// Allows system or monitoring actors to react to queue buildup
struct queue_threshold_warning : public message_base {
    std::string actor_name;
    size_t actor_instance_id = 0;
    size_t mailbox_size = 0;
    size_t ask_queue_size = 0;
    size_t total_size = 0;
    size_t threshold = 0;
};

// System message sent to watchers when a watched actor terminates
// Sent as part of the watch pattern for actor lifecycle monitoring
struct termination_msg : public message_base {
    std::string actor_name;
    size_t instance_id = 0;
    std::string type_name;
};

} // namespace cas

#endif // CAS_MESSAGE_BASE_H
