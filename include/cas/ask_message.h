#ifndef CAS_ASK_MESSAGE_H
#define CAS_ASK_MESSAGE_H

#include "message_base.h"
#include <future>
#include <tuple>
#include <typeindex>
#include <stdexcept>
#include <string>

namespace cas {

// Forward declaration
class actor;

// Thrown when ask() would block the very thread that must service the request.
// Ask requests are processed on the target actor's own worker thread, so a
// handler asking an actor that shares its thread would wait forever.
// Fix: use tell() with an explicit reply message instead of ask().
class ask_deadlock_error : public std::runtime_error {
public:
    explicit ask_deadlock_error(const std::string& what)
        : std::runtime_error(what) {}
};

// Thrown when an operation is attempted from on_start() that is not permitted
// during initialisation. on_start() is for initialising the actor itself:
// set_name(), handler registration, member setup, and schedule_* timers.
// tell() and system::create() are permitted. ask() is not - it blocks waiting
// for a reply from a system that may not be processing messages yet.
class on_start_violation : public std::logic_error {
public:
    explicit on_start_violation(const std::string& what)
        : std::logic_error(what) {}
};

// Thrown (to the ask() caller) when the call does not match the registered
// ask_handler: the decayed argument types differ from the handler's decayed
// parameter types, or ask<R>() names a different return type than the one
// registered. Arguments are not converted - pass exactly the parameter types,
// e.g. 10.0 rather than 10 for a double parameter, std::string{"x"} rather
// than "x" for a std::string parameter.
class ask_type_error : public std::logic_error {
public:
    explicit ask_type_error(const std::string& what)
        : std::logic_error(what) {}
};

// Internal message type for ask pattern
// Carries the operation tag, arguments, and a promise for the result
// This is only included in .cpp files, never in public headers
struct ask_request_base : public message_base {
    std::type_index op_type;  // Type of operation tag

    ask_request_base(std::type_index op) : op_type(op) {}
    virtual ~ask_request_base() = default;

private:
    framework_kind cas_framework_kind() const noexcept override { return framework_kind::ask_request; }

public:

    // Process this request by calling the appropriate handler
    // Implemented inline to avoid needing a .cpp file
    virtual void process(actor* target) = 0;
};

// Templated ask request for specific operation and args.
// Args are always decayed value types (actor_ref::ask instantiates this with
// std::decay_t of the caller's arguments): the request owns copies, so an
// lvalue argument cannot dangle if an ask-with-timeout caller returns before
// the handler runs, and the stored tuple type is comparable with the
// handler's registered args_type.
template<typename ReturnType, typename OpTag, typename... Args>
struct ask_request : public ask_request_base {
    static_assert((std::is_same_v<Args, std::decay_t<Args>> && ...),
                  "ask_request arguments must be decayed value types");

    std::tuple<Args...> args;  // Captured arguments (owned copies)
    std::promise<ReturnType> promise;  // Promise for result

    template<typename... CallerArgs>
    explicit ask_request(OpTag /*op*/, CallerArgs&&... a)
        : ask_request_base(typeid(OpTag))
        , args(std::forward<CallerArgs>(a)...)
    {}

    void process(actor* target) override;  // Implemented below after actor is fully defined
};

} // namespace cas

// Include actor.h to get full definition for template implementation
// This must come AFTER ask_request declaration but BEFORE implementation
#include "actor.h"

namespace cas {

// Template implementation - needs full actor definition
template<typename ReturnType, typename OpTag, typename... Args>
void ask_request<ReturnType, OpTag, Args...>::process(actor* target) {
    // Find handler in target actor
    auto& handlers = target->m_ask_handlers;
    auto it = handlers.find(op_type);

    if (it == handlers.end()) {
        // No handler registered - reject with exception
        promise.set_exception(
            std::make_exception_ptr(
                std::runtime_error("No ask handler registered for operation")
            )
        );
        return;
    }

    // The handler will static_cast &args to its own tuple type and the result
    // back to ReturnType. Both casts are only valid if the types registered by
    // ask_handler match this request exactly - verify before invoking.
    const auto& entry = it->second;
    if (entry.result_type != std::type_index(typeid(ReturnType))) {
        promise.set_exception(std::make_exception_ptr(ask_type_error(
            std::string("ask() return type mismatch for operation ") + typeid(OpTag).name() +
            ": caller asked for " + typeid(ReturnType).name() +
            ", handler returns " + entry.result_type.name())));
        return;
    }
    if (entry.args_type != std::type_index(typeid(std::tuple<Args...>))) {
        promise.set_exception(std::make_exception_ptr(ask_type_error(
            std::string("ask() argument type mismatch for operation ") + typeid(OpTag).name() +
            ": caller passed " + typeid(std::tuple<Args...>).name() +
            ", handler takes " + entry.args_type.name() +
            ". Arguments are not converted; pass exactly the parameter types.")));
        return;
    }

    // Call handler with args
    try {
        void* result_ptr = entry.invoke(&args);

        if constexpr (std::is_void_v<ReturnType>) {
            promise.set_value();
        } else {
            // Handler allocated result on heap, take ownership
            std::unique_ptr<ReturnType> result(static_cast<ReturnType*>(result_ptr));
            promise.set_value(std::move(*result));
        }
    } catch (...) {
        // Handler threw exception - propagate to caller
        promise.set_exception(std::current_exception());
    }
}

} // namespace cas

#endif // CAS_ASK_MESSAGE_H
