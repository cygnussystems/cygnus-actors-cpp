#ifndef CAS_ASK_MESSAGE_H
#define CAS_ASK_MESSAGE_H

#include "message_base.h"
#include <future>
#include <tuple>
#include <typeindex>

namespace cas {

// Forward declaration
class actor;

// Internal message type for ask pattern
// Carries the operation tag, arguments, and a promise for the result
// This is only included in .cpp files, never in public headers
struct ask_request_base : public message_base {
    std::type_index op_type;  // Type of operation tag

    ask_request_base(std::type_index op) : op_type(op) {}
    virtual ~ask_request_base() = default;

    // Process this request by calling the appropriate handler
    // Implemented inline to avoid needing a .cpp file
    virtual void process(actor* target) = 0;
};

// Templated ask request for specific operation and args
template<typename ReturnType, typename OpTag, typename... Args>
struct ask_request : public ask_request_base {
    std::tuple<Args...> args;  // Captured arguments
    std::promise<ReturnType> promise;  // Promise for result

    ask_request(OpTag /*op*/, Args&&... a)
        : ask_request_base(typeid(OpTag))
        , args(std::forward<Args>(a)...)
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

    // Call handler with args
    try {
        void* result_ptr = it->second(&args);

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
