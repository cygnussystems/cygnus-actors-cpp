#include "cas/actor.h"
#include "cas/actor_ref.h"
#include "cas/system.h"
#include "cas/actor_registry.h"
#include "cas/message_base.h"
#include "cas/ask_message.h"
#include <iostream>

namespace cas {

// Thread-local storage for current actor being processed
thread_local actor* current_actor_context = nullptr;

actor* actor::get_current_actor() {
    return current_actor_context;
}

const std::string& actor::name() const {
    return m_name;
}

size_t actor::instance_id() const {
    return system::get_instance_id(const_cast<actor*>(this));
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

    std::lock_guard<std::mutex> lock(m_mailbox_mutex);
    m_mailbox.push(std::move(msg));
    // TODO: Notify worker thread that message is available
}

void actor::enqueue_ask_message(std::unique_ptr<message_base> msg) {
    // Don't accept new messages if actor is stopping or stopped
    if (m_state.load() != actor_state::running) {
        return;  // Silently drop message
    }

    std::lock_guard<std::mutex> lock(m_ask_queue_mutex);
    m_ask_queue.push(std::move(msg));
    // TODO: Notify worker thread that priority message is available
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

    // Check ask queue first (priority)
    {
        std::lock_guard<std::mutex> lock(m_ask_queue_mutex);
        if (!m_ask_queue.empty()) {
            msg = std::move(m_ask_queue.front());
            m_ask_queue.pop();
        }
    }

    // If no priority message, check regular mailbox
    if (!msg) {
        std::lock_guard<std::mutex> lock(m_mailbox_mutex);
        if (!m_mailbox.empty()) {
            msg = std::move(m_mailbox.front());
            m_mailbox.pop();
        }
    }

    // Dispatch if we got a message
    if (msg) {
        // Set thread-local current actor context
        current_actor_context = this;
        dispatch_message(msg.get());
        current_actor_context = nullptr;
    }
}

bool actor::has_messages() {
    {
        std::lock_guard<std::mutex> lock(m_ask_queue_mutex);
        if (!m_ask_queue.empty()) {
            return true;
        }
    }  // Release ask_queue lock before acquiring mailbox lock

    {
        std::lock_guard<std::mutex> lock(m_mailbox_mutex);
        return !m_mailbox.empty();
    }
}

size_t actor::queue_size() const {
    size_t total = 0;

    {
        std::lock_guard<std::mutex> lock(m_ask_queue_mutex);
        total += m_ask_queue.size();
    }

    {
        std::lock_guard<std::mutex> lock(m_mailbox_mutex);
        total += m_mailbox.size();
    }

    return total;
}

actor_state actor::get_state() const {
    return m_state.load();
}

void actor::set_state(actor_state new_state) {
    m_state.store(new_state);
}

} // namespace cas
