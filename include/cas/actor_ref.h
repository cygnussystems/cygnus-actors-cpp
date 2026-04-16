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
    // Template implementations in actor_ref_impl.h (included after actor.h)
    template<typename MessageType>
    void tell(const MessageType& msg) const;

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

    // Check if actor reference is valid AND actor is running
    bool is_running() const;

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

} // namespace cas

#endif // CAS_ACTOR_REF_H

// Template implementations are in actor_ref_impl.h
// That file is included at the end of cas.h after actor.h is fully defined
