#ifndef CAS_MESSAGE_BASE_H
#define CAS_MESSAGE_BASE_H

#include "actor_ref.h"
#include "message_pool.h"
#include <cstdint>

namespace cas {

class actor;
class system;
namespace detail { class mailbox; }

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
        // Allocate extra space for the size header. The header is padded to
        // POOLED_ALIGNMENT and pool blocks come from ::operator new, so the
        // message after it keeps the default new alignment.
        constexpr size_t padded_header = message_pool::HEADER_OVERHEAD;

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
        constexpr size_t padded_header = message_pool::HEADER_OVERHEAD;

        void* raw = static_cast<char*>(ptr) - padded_header;
        size_t total_size = *static_cast<size_t*>(raw);

        message_pool::deallocate(raw, total_size);
    }

    // Over-aligned messages (alignas greater than POOLED_ALIGNMENT, e.g.
    // alignas(64) to keep a message on its own cache line) bypass the pool.
    // Pool blocks only have the default new alignment, and the size header
    // would shift the object off any stricter boundary. Without these
    // overloads a new-expression for such a type falls back to the unaligned
    // operator new above and returns a misaligned object.
    static void* operator new(size_t size, std::align_val_t alignment) {
        return ::operator new(size, alignment);
    }

    static void operator delete(void* ptr, std::align_val_t alignment) noexcept {
        ::operator delete(ptr, alignment);
    }

protected:
    // Internal: what the framework does with this message before looking for a
    // user handler. Identified by a virtual call rather than typeid or
    // dynamic_cast, which cost far more per message (on MSVC typeid comparison
    // and hashing work on the type's name). Overridden only by framework types.
    enum class framework_kind : unsigned char {
        user,          // dispatch to a registered handler
        shutdown,      // system_shutdown
        ask_request    // ask(): processed ahead of other messages
    };

private:
    friend class actor;
    friend class system;
    friend class detail::mailbox;

    virtual framework_kind cas_framework_kind() const noexcept { return framework_kind::user; }

    // Intrusive link used while the message sits in a mailbox (detail::mailbox).
    // Overwritten on every push, so a stale value copied along with a message
    // is harmless.
    message_base* m_next = nullptr;
};

// System message sent during shutdown
// Actors receive this and should call on_shutdown() and stop accepting external messages
struct system_shutdown : public message_base {
private:
    framework_kind cas_framework_kind() const noexcept override { return framework_kind::shutdown; }
};

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
