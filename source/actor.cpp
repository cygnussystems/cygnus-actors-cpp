#include "cas/actor.h"
#include "cas/actor_ref.h"
#include "cas/system.h"
#include "cas/actor_registry.h"
#include "cas/message_base.h"
#include "cas/ask_message.h"
#include "cas/actor_ref_impl.h"
#include <iostream>
#include <typeinfo>

// Platform-specific demangling
#ifdef _MSC_VER
    // MSVC doesn't mangle names in typeid, just use as-is
    #include <string>
#else
    // GCC/Clang need demangling
    #include <cxxabi.h>
    #include <memory>
#endif

namespace cas {

// Thread-local storage for current actor being processed
thread_local actor* current_actor_context = nullptr;

actor* actor::get_current_actor() {
    return current_actor_context;
}

const std::string& actor::name() const {
    // If no custom name set, generate default: typename_id
    if (m_name.empty()) {
        // Cache the auto-generated name
        const_cast<actor*>(this)->m_name = type_name() + "_" + std::to_string(m_instance_id);
    }
    return m_name;
}

size_t actor::instance_id() const {
    return m_instance_id;
}

std::string actor::type_name() const {
    const char* mangled = typeid(*this).name();

#ifdef _MSC_VER
    // MSVC: typeid().name() returns "class foo" or "struct foo"
    // Strip the "class " or "struct " prefix
    std::string full_name(mangled);
    if (full_name.compare(0, 6, "class ") == 0) {
        return full_name.substr(6);
    } else if (full_name.compare(0, 7, "struct ") == 0) {
        return full_name.substr(7);
    }
    return full_name;
#else
    // GCC/Clang: need to demangle
    int status = 0;
    std::unique_ptr<char, void(*)(void*)> demangled(
        abi::__cxa_demangle(mangled, nullptr, nullptr, &status),
        std::free
    );

    if (status == 0 && demangled) {
        return std::string(demangled.get());
    }

    // Fallback: return mangled name
    return std::string(mangled);
#endif
}

void actor::set_self_ref(std::shared_ptr<actor> self) {
    m_self_ref = self;
}

void actor::set_thread_affinity(size_t thread_id) {
    m_assigned_thread_id = thread_id;
}

size_t actor::get_thread_affinity() const {
    return m_assigned_thread_id;
}

void actor::set_queue_threshold(size_t threshold) {
    m_queue_threshold = threshold;
}

void actor::set_name(const std::string& name) {
    m_name = name;

    // Register with actor registry for name-based lookup
    if (auto shared = m_self_ref.lock()) {
        actor_registry::register_actor(name, shared);
    }
}

actor_ref actor::self() {
    if (auto shared = m_self_ref.lock()) {
        return actor_ref(shared);
    }
    // If self_ref is expired, this shouldn't happen in normal operation
    throw std::runtime_error("Actor self reference is no longer valid");
}

system* actor::get_system() {
    return system::get_instance();
}

void actor::enqueue_message(std::unique_ptr<message_base> msg) {
    // Don't accept new messages if actor is stopping or stopped
    if (m_state.load() != actor_state::running) {
        return;  // Silently drop message
    }

    // Lock-free enqueue
    m_mailbox.enqueue(std::move(msg));

    // Get approximate size for metrics and threshold checking
    size_t mailbox_size = m_mailbox.size_approx();

    // Update high water mark
    size_t current_hwm = m_mailbox_high_water_mark.load();
    while (mailbox_size > current_hwm &&
           !m_mailbox_high_water_mark.compare_exchange_weak(current_hwm, mailbox_size)) {
        // Loop until we successfully update or find that someone else updated to a higher value
    }

    // Check threshold
    if (m_queue_threshold > 0 && !m_threshold_warning_sent.load()) {
        size_t ask_size = m_ask_queue.size_approx();
        size_t total = mailbox_size + ask_size;

        if (total > m_queue_threshold) {
            // Send warning once per threshold breach
            bool expected = false;
            if (m_threshold_warning_sent.compare_exchange_strong(expected, true)) {
                // TODO: Send queue_threshold_warning message to system
                // For now, just log it
#ifdef CAS_DEBUG_LOGGING
                std::cout << "[QUEUE WARNING] Actor " << name() << " exceeded threshold: "
                          << total << " > " << m_queue_threshold << "\n" << std::flush;
#endif
            }
        }
    }

    // TODO: Notify worker thread that message is available
}

void actor::enqueue_ask_message(std::unique_ptr<message_base> msg) {
    // Don't accept new messages if actor is stopping or stopped
    if (m_state.load() != actor_state::running) {
        return;  // Silently drop message
    }

    // Enqueue to global ask queue (processed by dedicated ask threads)
    system::enqueue_global_ask(this, std::move(msg));

    // Note: We no longer track ask queue metrics locally since ask requests
    // are handled by dedicated threads and don't go through actor's local queue
}

void actor::dispatch_message(message_base* msg) {
#ifdef CAS_DEBUG_LOGGING
    std::cout << "[DISPATCH:" << m_name << "] Dispatching message, typeid: " << typeid(*msg).name() << "\n" << std::flush;

    // Debug: print all registered handlers
    std::cout << "[DISPATCH:" << m_name << "] Registered handlers count: " << m_handlers.size() << "\n" << std::flush;
    for (const auto& [type_idx, handler] : m_handlers) {
        std::cout << "[DISPATCH:" << m_name << "]   - " << type_idx.name() << "\n" << std::flush;
    }
#endif

    // Check for system shutdown message first
    if (typeid(*msg) == typeid(system_shutdown)) {
        // System shutdown - call on_shutdown() and set state
        on_shutdown();
        set_state(actor_state::stopping);
        return;
    }

    // Check for ask request (RPC-style call)
    if (ask_request_base* ask_msg = dynamic_cast<ask_request_base*>(msg)) {
        // Process ask request (calls handler and sets promise)
        ask_msg->process(this);
        return;
    }

    auto it = m_handlers.find(typeid(*msg));
    if (it != m_handlers.end()) {
        // Found handler - call it
#ifdef CAS_DEBUG_LOGGING
        std::cout << "[DISPATCH:" << m_name << "] Handler found, calling...\n" << std::flush;
#endif
        it->second(msg);
#ifdef CAS_DEBUG_LOGGING
        std::cout << "[DISPATCH:" << m_name << "] Handler completed\n" << std::flush;
#endif
    } else {
        // No handler registered for this message type
#ifdef CAS_DEBUG_LOGGING
        std::cout << "[DISPATCH:" << m_name << "] No handler found!\n" << std::flush;
#endif
        on_unhandled_message(msg);
    }
}

void actor::process_next_message() {
    std::unique_ptr<message_base> msg;

    // Try to dequeue from mailbox
    // Note: Ask requests are now processed by dedicated ask threads,
    // so regular workers only process fire-and-forget messages
    if (m_mailbox.try_dequeue(msg)) {
        // Got a message - dispatch it
        current_actor_context = this;
        dispatch_message(msg.get());
        current_actor_context = nullptr;
    }
}

bool actor::has_messages() {
    // Check mailbox only (ask requests handled by dedicated threads)
    return m_mailbox.size_approx() > 0;
}

size_t actor::queue_size() const {
    // Return mailbox size only (ask requests handled by dedicated threads)
    return m_mailbox.size_approx();
}

actor_state actor::get_state() const {
    return m_state.load();
}

void actor::set_state(actor_state new_state) {
    m_state.store(new_state);
}

size_t actor::mailbox_high_water_mark() const {
    return m_mailbox_high_water_mark.load();
}

size_t actor::ask_queue_high_water_mark() const {
    return m_ask_queue_high_water_mark.load();
}

size_t actor::total_high_water_mark() const {
    return m_mailbox_high_water_mark.load() + m_ask_queue_high_water_mark.load();
}

void actor::cancel_timer(timer_id id) {
    if (id == INVALID_TIMER_ID) {
        return;  // No-op for invalid ID
    }

    // Remove from our tracking set
    m_active_timers.erase(id);

    // Cancel in system
    system::cancel_timer_internal(id);
}

} // namespace cas
