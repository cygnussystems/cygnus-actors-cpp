#ifndef CAS_ACTOR_REF_H
#define CAS_ACTOR_REF_H

#include <memory>
#include <future>
#include <chrono>
#include <optional>

namespace cas {

// Forward declarations
class actor;
class system;
struct message_base;

// Reference/handle to an actor
// Used to send messages to actors in a thread-safe way
class actor_ref {
private:
    std::shared_ptr<actor> m_actor_ptr;

public:
    actor_ref() = default;
    explicit actor_ref(std::shared_ptr<actor> ptr);

    // Fire-and-forget: Send a message to this actor (enqueue to mailbox)
    template<typename MessageType>
    void receive(const MessageType& msg) const;

    template<typename MessageType>
    void push(const MessageType& msg) const;

    template<typename MessageType>
    void enqueue(const MessageType& msg) const;

    // Operator overload for convenient syntax
    template<typename MessageType>
    const actor_ref& operator<<(const MessageType& msg) const;

    // RPC-style ask: Call a function on the actor and block for result
    // Usage: auto result = actor_ref.ask<double>(profit_op{}, 42, 5);
    template<typename ReturnType, typename OpTag, typename... Args>
    ReturnType ask(OpTag op, Args&&... args);

    // RPC-style ask with timeout (returns optional)
    // Usage: auto opt = actor_ref.ask<double>(profit_op{}, 5s, 42, 5);
    template<typename ReturnType, typename OpTag, typename... Args>
    std::optional<ReturnType> ask(OpTag op, std::chrono::milliseconds timeout, Args&&... args);

    // Check if reference is valid
    bool is_valid() const;
    explicit operator bool() const;

    // Get actor name (for testing and debugging)
    std::string name() const;

    // Access underlying actor pointer (internal use)
    std::shared_ptr<actor> get_actor() const;

    // Get typed access to the actor (for testing)
    // Returns nullptr if the ref is invalid or the type doesn't match
    template<typename ActorType>
    ActorType* get() const {
        if (!m_actor_ptr) return nullptr;
        return dynamic_cast<ActorType*>(m_actor_ptr.get());
    }

    // Get typed access with runtime check (for testing)
    // Returns reference, throws std::runtime_error if invalid or wrong type
    template<typename ActorType>
    ActorType& get_checked() const {
        if (!m_actor_ptr) {
            throw std::runtime_error("Cannot get_checked() on invalid actor_ref");
        }
        ActorType* typed_ptr = dynamic_cast<ActorType*>(m_actor_ptr.get());
        if (!typed_ptr) {
            throw std::runtime_error("actor_ref type mismatch in get_checked()");
        }
        return *typed_ptr;
    }

    // Comparison operators (for using in maps, sets, etc.)
    bool operator==(const actor_ref& other) const;
    bool operator!=(const actor_ref& other) const;
};

// Forward declare ask_request for template use
template<typename ReturnType, typename OpTag, typename... Args>
struct ask_request;

// Template implementations (must be in header)

template<typename MessageType>
void actor_ref::receive(const MessageType& msg) const {
    if (!m_actor_ptr) return;

    // Clone message from stack and enqueue
    auto msg_copy = std::make_unique<MessageType>(msg);

    // Set sender to current actor (thread-local context)
    // If called from within an actor's message handler, this will be that actor
    // If called from outside (e.g., main), sender will be invalid (nullptr actor_ptr)
    actor* current = actor::get_current_actor();
    if (current) {
        msg_copy->sender = current->self();
    }
    // else: sender remains default-constructed (invalid actor_ref)

    // Assign unique message ID
    msg_copy->message_id = system::next_message_id();

    // correlation_id remains as set by user (0 if not a reply)

    m_actor_ptr->enqueue_message(std::move(msg_copy));
}

template<typename MessageType>
void actor_ref::push(const MessageType& msg) const {
    receive(msg);  // Alias for receive
}

template<typename MessageType>
void actor_ref::enqueue(const MessageType& msg) const {
    receive(msg);  // Alias for receive
}

template<typename MessageType>
const actor_ref& actor_ref::operator<<(const MessageType& msg) const {
    receive(msg);
    return *this;
}

template<typename ReturnType, typename OpTag, typename... Args>
ReturnType actor_ref::ask(OpTag op, Args&&... args) {
    if (!m_actor_ptr) {
        throw std::runtime_error("Cannot ask on invalid actor_ref");
    }

    // Create ask request with promise/future
    auto request = std::make_unique<ask_request<ReturnType, OpTag, Args...>>(
        op, std::forward<Args>(args)...
    );

    // Get future before moving request
    std::future<ReturnType> future = request->promise.get_future();

    // Set sender
    actor* current = actor::get_current_actor();
    if (current) {
        request->sender = current->self();
    }

    // Assign message ID
    request->message_id = system::next_message_id();

    // Enqueue to ask queue (priority)
    m_actor_ptr->enqueue_ask_message(std::move(request));

    // Block waiting for result
    return future.get();
}

template<typename ReturnType, typename OpTag, typename... Args>
std::optional<ReturnType> actor_ref::ask(OpTag op, std::chrono::milliseconds timeout, Args&&... args) {
    if (!m_actor_ptr) {
        return std::nullopt;
    }

    // Create ask request with promise/future
    auto request = std::make_unique<ask_request<ReturnType, OpTag, Args...>>(
        op, std::forward<Args>(args)...
    );

    // Get future before moving request
    std::future<ReturnType> future = request->promise.get_future();

    // Set sender
    actor* current = actor::get_current_actor();
    if (current) {
        request->sender = current->self();
    }

    // Assign message ID
    request->message_id = system::next_message_id();

    // Enqueue to ask queue (priority)
    m_actor_ptr->enqueue_ask_message(std::move(request));

    // Wait with timeout
    if (future.wait_for(timeout) == std::future_status::ready) {
        return future.get();
    } else {
        return std::nullopt;  // Timeout
    }
}

} // namespace cas

#endif // CAS_ACTOR_REF_H

// NOTE: To use actor_ref::ask(), you must include "ask_message.h" in your .cpp file
// after including "actor_ref.h". This avoids circular dependency issues.
