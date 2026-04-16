#ifndef CAS_MESSAGE_BASE_H
#define CAS_MESSAGE_BASE_H

#include "actor_ref.h"
#include "message_pool.h"
#include <cstdint>

namespace cas {

// Base struct for all messages
// Messages inherit from this to get sender information and correlation tracking
// Uses pooled allocation via overridden operator new/delete for performance
struct message_base {
    actor_ref sender;           // Who sent this message (set by framework)
    uint64_t message_id = 0;    // Unique ID for this message (set by framework)
    uint64_t correlation_id = 0; // ID of message this is replying to (0 = not a reply)
    virtual ~message_base() = default;  // Make polymorphic to preserve type info

    // Pooled allocation - automatically used by make_unique and new
    // Size is stored in header before the returned pointer
    static void* operator new(size_t size) {
        // Allocate extra space for size header
        constexpr size_t header_size = sizeof(size_t);
        constexpr size_t alignment = alignof(std::max_align_t);
        size_t padded_header = (header_size + alignment - 1) & ~(alignment - 1);

        size_t total_size = padded_header + size;
        void* raw = message_pool::allocate(total_size);

        // Store size in header
        *static_cast<size_t*>(raw) = total_size;

        // Return pointer past the header
        return static_cast<char*>(raw) + padded_header;
    }

    static void operator delete(void* ptr) noexcept {
        if (ptr == nullptr) return;

        // Recover header to get size
        constexpr size_t header_size = sizeof(size_t);
        constexpr size_t alignment = alignof(std::max_align_t);
        size_t padded_header = (header_size + alignment - 1) & ~(alignment - 1);

        void* raw = static_cast<char*>(ptr) - padded_header;
        size_t total_size = *static_cast<size_t*>(raw);

        message_pool::deallocate(raw, total_size);
    }
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
