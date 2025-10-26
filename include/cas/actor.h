#ifndef CAS_ACTOR_H
#define CAS_ACTOR_H

#include <string>
#include <memory>
#include <queue>
#include <mutex>
#include <unordered_map>
#include <typeindex>
#include <functional>
#include <atomic>
#include <iostream>

// Uncomment to enable debug logging
// #define CAS_DEBUG_LOGGING

namespace cas {

// Forward declarations
class actor_ref;
class system;
struct message_base;
struct ask_request_base;  // For friend declaration

// Actor lifecycle state
enum class actor_state {
    running,   // Normal operation, accepting messages
    stopping,  // Shutdown initiated, draining messages, no new messages accepted
    stopped    // Fully stopped, on_stop() has been called
};

// Base class for all actors
// Users inherit from this and override lifecycle hooks and message handlers
class actor {
    // Allow system to call protected lifecycle methods
    friend class system;
    // Allow ask requests to access handlers
    friend struct ask_request_base;

private:
    std::string m_name;
    std::weak_ptr<actor> m_self_ref;  // Weak reference to self for creating actor_ref

    // Thread affinity - which thread processes this actor
    // Only that thread reads from queues, so no lock needed for reading
    size_t m_assigned_thread_id = 0;

    // Lifecycle state - atomic so it can be checked without locking
    std::atomic<actor_state> m_state{actor_state::running};

    // Two queues for different message priorities
    // Regular mailbox: fire-and-forget messages (receive/push/enqueue)
    // Multiple threads can SEND (write), but only assigned thread READS
    std::queue<std::unique_ptr<message_base>> m_mailbox;
    mutable std::mutex m_mailbox_mutex;  // Mutable for const methods like queue_size()

    // Ask queue: priority request-response messages (ask)
    // Processed before regular mailbox to provide RPC-like semantics
    std::queue<std::unique_ptr<message_base>> m_ask_queue;
    mutable std::mutex m_ask_queue_mutex;  // Mutable for const methods like queue_size()

    // Message type -> handler function map
    std::unordered_map<std::type_index, std::function<void(message_base*)>> m_handlers;

    // Ask operation type -> handler function map
    // Maps operation tag type to a handler that processes args and returns result
    std::unordered_map<std::type_index, std::function<void*(void*)>> m_ask_handlers;

    // Unique instance ID (assigned by system during registration)
    // Placed at end with explicit alignment to minimize layout impact
    alignas(8) size_t m_instance_id = 0;

    // Queue metrics - track high water marks
    std::atomic<size_t> m_mailbox_high_water_mark{0};
    std::atomic<size_t> m_ask_queue_high_water_mark{0};

    // Queue threshold - warn if total queue exceeds this (0 = disabled)
    size_t m_queue_threshold = 0;

    // Track if we've already warned about queue threshold to avoid spam
    std::atomic<bool> m_threshold_warning_sent{false};

protected:
    // Lifecycle hooks - override these
    // on_start() is required - use it to register message handlers and initialize state
    virtual void on_start() = 0;

    // Called when shutdown is initiated, before message draining
    // Actors can send final messages here (e.g., cleanup notifications)
    virtual void on_shutdown() {}

    virtual void on_stop() {}

    // Called when a message has no registered handler
    virtual void on_unhandled_message(message_base* /*msg*/) {
        // Default: ignore (could log warning in debug mode)
    }

    // Acto-style handler registration API for messages
    // Register a handler using a member function pointer
    // Usage: handler<message::ping>(&my_actor::on_ping);
    template<typename MessageType, typename ActorType>
    void handler(void (ActorType::*method)(const MessageType&)) {
#ifdef CAS_DEBUG_LOGGING
        std::cout << "[HANDLER REG] Registering member function handler for: " << typeid(MessageType).name() << "\n" << std::flush;
#endif
        m_handlers[typeid(MessageType)] = [this, method](message_base* base_msg) {
            MessageType* msg = static_cast<MessageType*>(base_msg);
            (static_cast<ActorType*>(this)->*method)(*msg);
        };
    }

    // Register a handler using a lambda or function object
    // Usage: handler<message::ping>([this](const message::ping& msg) { ... });
    template<typename MessageType, typename Callable>
    void handler(Callable&& callable) {
#ifdef CAS_DEBUG_LOGGING
        std::cout << "[HANDLER REG] Registering lambda handler for: " << typeid(MessageType).name() << "\n" << std::flush;
#endif
        m_handlers[typeid(MessageType)] = [callable = std::forward<Callable>(callable)](message_base* base_msg) {
            MessageType* msg = static_cast<MessageType*>(base_msg);
            callable(*msg);
        };
    }

    // Register an ask handler (RPC-style function call)
    // Usage: ask_handler<double, profit_op>(&my_actor::calculate_profit);
    template<typename ReturnType, typename OpTag, typename ActorType, typename... Args>
    void ask_handler(ReturnType (ActorType::*method)(Args...)) {
        m_ask_handlers[typeid(OpTag)] = [this, method](void* args_ptr) -> void* {
            // Unpack arguments from tuple
            auto* args = static_cast<std::tuple<Args...>*>(args_ptr);

            // Call member function with unpacked args
            if constexpr (std::is_void_v<ReturnType>) {
                std::apply([this, method](auto&&... unpacked_args) {
                    (static_cast<ActorType*>(this)->*method)(std::forward<decltype(unpacked_args)>(unpacked_args)...);
                }, *args);
                return nullptr;
            } else {
                auto result = std::apply([this, method](auto&&... unpacked_args) {
                    return (static_cast<ActorType*>(this)->*method)(std::forward<decltype(unpacked_args)>(unpacked_args)...);
                }, *args);
                return new ReturnType(std::move(result));
            }
        };
    }

    // Set the actor's name (typically called in on_start)
    // Also registers with actor_registry for name-based lookup
    void set_name(const std::string& name);

    // Get reference to the system
    system* get_system();

public:
    actor() = default;
    virtual ~actor() = default;

    // Non-copyable, non-movable
    actor(const actor&) = delete;
    actor& operator=(const actor&) = delete;
    actor(actor&&) = delete;
    actor& operator=(actor&&) = delete;

    // Get actor name (user-set or auto-generated type_id)
    const std::string& name() const;

    // Get actor type name (demangled class name from RTTI)
    std::string type_name() const;

    // Get current actor being processed (thread-local)
    static actor* get_current_actor();

    // Get unique instance ID (assigned during actor creation)
    size_t instance_id() const;

    // Internal: set self reference (called by framework)
    void set_self_ref(std::shared_ptr<actor> self);

    // Internal: set thread affinity (called by framework)
    void set_thread_affinity(size_t thread_id);

    // Internal: get thread affinity
    size_t get_thread_affinity() const;

    // Internal: set queue threshold (called by framework)
    void set_queue_threshold(size_t threshold);

    // Get an actor_ref to this actor
    actor_ref self();

    // Internal: enqueue regular message (called by actor_ref::receive/push/enqueue)
    virtual void enqueue_message(std::unique_ptr<message_base> msg);

    // Internal: enqueue ask message (called by actor_ref::ask)
    virtual void enqueue_ask_message(std::unique_ptr<message_base> msg);

    // Internal: dispatch message to correct handler
    void dispatch_message(message_base* msg);

    // Internal: process next message (checks ask queue first, then mailbox)
    virtual void process_next_message();

    // Internal: check if actor has any messages (ask queue or mailbox)
    virtual bool has_messages();

    // Internal: get total number of queued messages (for shutdown monitoring)
    virtual size_t queue_size() const;

    // Internal: get current state
    actor_state get_state() const;

    // Internal: set state (called by system during shutdown)
    void set_state(actor_state new_state);

    // Queue metrics - get high water marks
    size_t mailbox_high_water_mark() const;
    size_t ask_queue_high_water_mark() const;
    size_t total_high_water_mark() const;
};

// Thread-local storage for current actor being processed (defined in actor.cpp)
extern thread_local actor* current_actor_context;

} // namespace cas

#endif // CAS_ACTOR_H
