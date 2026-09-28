#include "cas/actor.h"
#include "cas/actor_ref.h"
#include "cas/system.h"
#include "cas/actor_registry.h"
#include "cas/message_base.h"
#include "cas/ask_message.h"
#include "cas/actor_ref_impl.h"
#include <iostream>
#include <typeinfo>
#include <cstdlib>

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

bool actor::is_initialising() const {
    return m_in_on_start;
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
    actor_state current_state = m_state.load();
    if (current_state != actor_state::running) {
        // Report dead letter before dropping
        system::report_dead_letter(name(), msg.get(), current_state, false);
        return;  // Drop message
    }

    // Lock-free enqueue. Closed means the actor finished stopping after the
    // state check above: the message was never queued, so it is a dead letter.
    if (m_mailbox.push(msg) == detail::push_result::closed) {
        system::report_dead_letter(name(), msg.get(), m_state.load(), false);
        return;
    }

    // Approximate size for metrics and threshold checking
    size_t mailbox_size = m_mailbox.other_size();

    // Update high water mark
    size_t current_hwm = m_mailbox_high_water_mark.load();
    while (mailbox_size > current_hwm &&
           !m_mailbox_high_water_mark.compare_exchange_weak(current_hwm, mailbox_size)) {
        // Loop until we successfully update or find that someone else updated to a higher value
    }

    // Check threshold
    if (m_queue_threshold > 0 && !m_threshold_warning_sent.load()) {
        size_t total = m_mailbox.size();

        if (total > m_queue_threshold) {
            // Report once per threshold breach
            bool expected = false;
            if (m_threshold_warning_sent.compare_exchange_strong(expected, true)) {
#ifdef CAS_DEBUG_LOGGING
                std::cout << "[QUEUE WARNING] Actor " << name() << " exceeded threshold: "
                          << total << " > " << m_queue_threshold << "\n" << std::flush;
#endif
                // Deliver to the user's handler (works in release builds).
                system::report_queue_threshold(name(), total, m_queue_threshold);
            }
        }
    }

    // Wake the worker thread owning this actor
    system::notify_worker(m_assigned_thread_id);
}

void actor::enqueue_ask_message(std::unique_ptr<message_base> msg) {
    // Don't accept new messages if actor is stopping or stopped
    actor_state current_state = m_state.load();
    if (current_state != actor_state::running) {
        // Report dead letter before dropping
        system::report_dead_letter(name(), msg.get(), current_state, true);
        return;  // Drop message
    }

    // Ask requests share the mailbox with every other message and are
    // delivered in order with them (INVARIANT 3): an ask never overtakes
    // messages its sender sent earlier. Consumed by process_next_message() on
    // the owning thread, so ask handlers are serialised with regular handlers
    // (INVARIANT 1). The caller blocks on the request's future until then.
    if (m_mailbox.push(msg) == detail::push_result::closed) {
        system::report_dead_letter(name(), msg.get(), m_state.load(), true);
        return;  // destroying the request breaks its promise; the caller is not left hanging
    }

    // Update ask queue high water mark
    size_t ask_size = m_mailbox.ask_size();
    size_t current_hwm = m_ask_queue_high_water_mark.load();
    while (ask_size > current_hwm &&
           !m_ask_queue_high_water_mark.compare_exchange_weak(current_hwm, ask_size)) {
        // Loop until we successfully update or find that someone else updated higher
    }

    // Wake the worker thread owning this actor - the caller is blocked on the
    // request's future until that thread drains the ask queue.
    system::notify_worker(m_assigned_thread_id);
}

namespace {

#ifndef NDEBUG
// RAII guard asserting that only one thread executes a given actor at a time.
// Backstop for INVARIANT 1; compiled out entirely in release builds.
class dispatch_guard {
public:
    explicit dispatch_guard(std::atomic<bool>& flag, const std::string& actor_name)
        : m_flag(flag)
    {
        bool expected = false;
        if (!m_flag.compare_exchange_strong(expected, true)) {
            std::cerr << "[CAS FATAL] INVARIANT 1 VIOLATED: actor '" << actor_name
                      << "' is being dispatched by two threads concurrently.\n"
                      << "  An actor's handlers must run on one thread at a time.\n"
                      << "  See doc/INVARIANTS.md.\n" << std::flush;
            std::abort();
        }
    }

    ~dispatch_guard() {
        m_flag.store(false);
    }

    dispatch_guard(const dispatch_guard&) = delete;
    dispatch_guard& operator=(const dispatch_guard&) = delete;

private:
    std::atomic<bool>& m_flag;
};
#endif

} // namespace

void actor::dispatch_message(message_base* msg) {
#ifndef NDEBUG
    dispatch_guard guard(m_in_dispatch, name());
#endif

#ifdef CAS_DEBUG_LOGGING
    std::cout << "[DISPATCH:" << m_name << "] Dispatching message, typeid: " << typeid(*msg).name() << "\n" << std::flush;

    // Debug: print all registered handlers
    std::cout << "[DISPATCH:" << m_name << "] Registered handlers count: " << m_handlers.size() << "\n" << std::flush;
    for (const auto& [type_idx, handler] : m_handlers) {
        std::cout << "[DISPATCH:" << m_name << "]   - " << type_idx.name() << "\n" << std::flush;
    }
#endif

    // Framework messages first. Identified by a virtual call: typeid and
    // dynamic_cast here cost more than the rest of dispatch put together.
    const auto kind = msg->cas_framework_kind();

    if (kind == message_base::framework_kind::shutdown) {
        // System shutdown owns this actor's on_shutdown() only if no
        // stop_actor() has claimed it. If one has, that stop runs the hooks
        // (on this thread) and the shutdown message is simply consumed -
        // otherwise on_shutdown() would run twice.
        stop_phase expected = stop_phase::none;
        if (!m_stop_phase.compare_exchange_strong(expected, stop_phase::system_shutdown)) {
            return;
        }
        on_shutdown();
        set_state(actor_state::stopping);
        return;
    }

    // Ask request (RPC-style call): calls the ask handler and sets the promise
    if (kind == message_base::framework_kind::ask_request) {
        static_cast<ask_request_base*>(msg)->process(this);
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
    // Delivery order is send order per sender (INVARIANT 3), asks included.
    // Consumed only here, on the owning thread, so an ask handler never runs
    // concurrently with a regular handler (INVARIANT 1).
    if (std::unique_ptr<message_base> msg = m_mailbox.pop()) {
        // Got a message - dispatch it
        actor* prev_context = current_actor_context;
        current_actor_context = this;
        dispatch_message(msg.get());
        current_actor_context = prev_context;
    }
}

bool actor::has_messages() {
    // Approximate under concurrent producers (see detail::mailbox::size()).
    // Adequate for polling and metrics; shutdown drain also bounds by timeout.
    return m_mailbox.size() > 0;
}

size_t actor::queue_size() const {
    // Approximate - see has_messages().
    return m_mailbox.size();
}

actor_state actor::get_state() const {
    return m_state.load();
}

void actor::set_state(actor_state new_state) {
    m_state.store(new_state);
    // system::stop_actor() blocks in m_state.wait() until the actor is stopped
    m_state.notify_all();
}

actor::stop_phase actor::current_stop_phase() const {
    return m_stop_phase.load(std::memory_order_acquire);
}

void actor::report_rejected(const message_base* msg, bool was_ask) {
    system::report_dead_letter(name(), msg, m_state.load(), was_ask);
}

bool actor::service_stop_request() {
    return system::service_stop(this);
}

void actor::discard_pending_messages() {
    // Destroying an ask request breaks its promise: a blocked ask() caller gets
    // std::future_error (broken_promise) rather than hanging. A pending
    // system_shutdown is framework control traffic, not a message anyone sent
    // this actor - not a dead letter.
    while (std::unique_ptr<message_base> msg = m_mailbox.pop()) {
        const auto kind = msg->cas_framework_kind();
        if (kind != message_base::framework_kind::shutdown) {
            report_rejected(msg.get(), kind == message_base::framework_kind::ask_request);
        }
    }
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
