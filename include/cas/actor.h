#ifndef CAS_ACTOR_H
#define CAS_ACTOR_H

#include <string>
#include <memory>
#include <unordered_map>
#include <typeindex>
#include <functional>
#include <atomic>
#include <iostream>
#include <chrono>
#include <set>
#include <concepts>
#include <type_traits>
#include "timer.h"
#include "external/concurrentqueue.h"

// Uncomment to enable debug logging
// #define CAS_DEBUG_LOGGING

namespace cas {

// Forward declarations
class actor_ref;
class system;
struct message_base;
struct ask_request_base;  // For friend declaration
template<typename ReturnType, typename OpTag, typename... Args> struct ask_request;  // For friend declaration

// Subclasses granted dispatch access (see friend declarations below)
template<bool ThreadSafe> class inline_actor;
class stateful_actor;
class zeromq_relay_actor;

// Constrains the callable passed to actor::handler(). Catches a mismatched
// lambda signature at the registration call site, instead of deep inside the
// std::function instantiation it would otherwise be stored in.
//
// Deliberately expressed as invocability rather than an exact parameter match:
// generic handlers written as [](const auto& msg) are used in the framework
// itself (see source/zeromq_relay_actor.cpp) and must keep working.
//
// Note this accepts a by-value parameter too: [](my_msg m) satisfies it and
// silently copies each message on dispatch. That is deliberate - taking a
// message by value is legitimate, and rejecting it would turn a performance
// preference into a compile error.
template<typename Callable, typename MessageType>
concept message_handler_for = std::invocable<Callable, const MessageType&>;

// Constrains the message type a handler can be registered for.
//
// Only types that can actually reach a mailbox are accepted: messages arrive
// as unique_ptr<message_base>, so a handler registered for an unrelated type
// could never be invoked. Without this, handler<int>(...) compiles and then
// sits silently unreachable.
//
// message_base is complete here despite only being forward-declared above:
// actor.h includes timer.h, which includes message_base.h.
//
// derived_from<M, message_base> is satisfied when M *is* message_base, which
// is required - tests/01_simple/test_handler_registration.cpp registers a
// handler for message_base itself. Do not tighten this to a proper-base check.
template<typename MessageType>
concept message_type = std::derived_from<MessageType, message_base>;

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
    template<typename ReturnType, typename OpTag, typename... Args> friend struct ask_request;

    // Subclasses that override process_next_message() and therefore own the
    // actor's execution turn themselves. Each must dispatch only from the
    // actor's owning thread - see INVARIANT 1 in doc/INVARIANTS.md.
    template<bool ThreadSafe> friend class inline_actor;
    friend class stateful_actor;
    friend class zeromq_relay_actor;

    // Sets m_in_on_start for the duration of on_start()
    friend class on_start_scope;

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
    // Lock-free MPMC queue - multiple threads can send, assigned thread reads
    moodycamel::ConcurrentQueue<std::unique_ptr<message_base>> m_mailbox;

    // Ask queue: priority request-response messages (ask)
    // Processed before regular mailbox to provide RPC-like semantics.
    // INVARIANT: drained only by process_next_message() on the owning thread,
    // so ask handlers are serialised with regular handlers. See doc/INVARIANTS.md.
    moodycamel::ConcurrentQueue<std::unique_ptr<message_base>> m_ask_queue;

    // Message type -> handler function map
    std::unordered_map<std::type_index, std::function<void(message_base*)>> m_handlers;

    // Ask operation type -> handler function map
    // Maps operation tag type to a handler that processes args and returns result
    std::unordered_map<std::type_index, std::function<void*(void*)>> m_ask_handlers;

    // Unique instance ID (assigned by system during registration)
    // Placed at end with explicit alignment to minimize layout impact
    alignas(8) size_t m_instance_id = 0;

    // Active timer IDs for this actor (for lifecycle management)
    std::set<timer_id> m_active_timers;

    // Queue metrics - track high water marks
    std::atomic<size_t> m_mailbox_high_water_mark{0};
    std::atomic<size_t> m_ask_queue_high_water_mark{0};

    // Queue threshold - warn if total queue exceeds this (0 = disabled)
    size_t m_queue_threshold = 0;

    // Track if we've already warned about queue threshold to avoid spam
    std::atomic<bool> m_threshold_warning_sent{false};

    // True while this actor's on_start() is executing. Used to reject ask()
    // from initialisation, where no worker is guaranteed to be servicing the
    // target yet. Only ever read/written by the thread running on_start().
    bool m_in_on_start = false;

#ifndef NDEBUG
    // Debug-only guard: detects two threads executing this actor concurrently.
    // Backstop for INVARIANT 1 - catches violations that slip past the
    // compile-time encapsulation of dispatch_message().
    std::atomic<bool> m_in_dispatch{false};
#endif

    // INVARIANT 1: exactly one thread may execute an actor's handlers at any
    // instant. dispatch_message() is private so the set of callers is closed
    // and greppable; every caller below must hold the actor's execution turn.
    // Adding a friend here is a threading-model change - see doc/INVARIANTS.md.
    void dispatch_message(message_base* msg);

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
        requires message_type<MessageType> && std::derived_from<ActorType, actor>
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
        requires message_type<MessageType> && message_handler_for<Callable, MessageType>
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
    //
    // The derived_from check reports a method belonging to an unrelated class
    // at this call site. Without it the mismatch surfaces from inside the
    // lambda below, where the actor has already been cast through void* and
    // std::apply, and the diagnostic is close to unreadable.
    template<typename ReturnType, typename OpTag, typename ActorType, typename... Args>
        requires std::derived_from<ActorType, actor>
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

    // Timer API: Schedule messages to be sent in the future

    // Schedule a message to be sent once after a delay
    // Returns a timer_id that can be used to cancel the timer
    template<typename MessageType>
    timer_id schedule_once(std::chrono::milliseconds delay, MessageType msg);

    // Schedule a message to be sent repeatedly at intervals
    // Returns a timer_id that can be used to cancel the timer
    template<typename MessageType>
    timer_id schedule_periodic(std::chrono::milliseconds interval, MessageType msg);

    // Cancel a scheduled timer
    // Safe to call with already-fired or cancelled timers (no-op)
    void cancel_timer(timer_id id);

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

    // True while this actor's on_start() is running. Operations that would
    // block on the message system are rejected during initialisation.
    bool is_initialising() const;

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

    // Internal: process next message (checks ask queue first, then mailbox)
    // Runs on the actor's owning thread only - this is the actor's execution turn.
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

// Internal: RAII helper that sets the actor context and marks the actor as
// initialising for the duration of on_start(). Used by system::start(),
// system::register_actor() and fast_actor's thread entry so the on_start()
// contract is enforced identically on every path.
class on_start_scope {
public:
    explicit on_start_scope(actor* self)
        : m_self(self), m_prev(current_actor_context)
    {
        current_actor_context = self;
        self->m_in_on_start = true;
    }

    ~on_start_scope() {
        m_self->m_in_on_start = false;
        current_actor_context = m_prev;
    }

    on_start_scope(const on_start_scope&) = delete;
    on_start_scope& operator=(const on_start_scope&) = delete;

private:
    actor* m_self;
    actor* m_prev;
};

} // namespace cas

// Template implementations are in actor_impl.h (included by cas.h after system.h)

#endif // CAS_ACTOR_H
