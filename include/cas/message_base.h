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

} // namespace cas

#endif // CAS_MESSAGE_BASE_H
