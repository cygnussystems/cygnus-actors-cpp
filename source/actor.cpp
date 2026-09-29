#include "cas/actor.h"
#include "cas/actor_ref.h"
#include "cas/system.h"
#include "cas/actor_registry.h"
#include "cas/message_base.h"
#include "cas/call_message.h"
#include "cas/actor_ref_impl.h"
#include <iostream>
#include <typeinfo>
#include <algorithm>
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
    if (const std::string* n = m_name.load(std::memory_order_acquire)) {
        return *n;
    }
    // Only reachable before system::register_actor() - the actor is not
    // shared with any other thread yet, so writing here cannot race.
    const_cast<actor*>(this)->assign_default_name();
    return *m_name.load(std::memory_order_acquire);
}

void actor::publish_name(std::string name) {
    m_names.push_front(std::move(name));
    m_name.store(&m_names.front(), std::memory_order_release);
}

void actor::assign_default_name() {
    if (m_name.load(std::memory_order_relaxed) == nullptr) {
        publish_name(type_name() + "_" + std::to_string(m_instance_id));
    }
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

bool actor::runs_inline() const {
    return false;
}

void actor::set_queue_threshold(size_t threshold) {
    m_queue_threshold = threshold;
}

void actor::set_name(const std::string& name) {
    const std::string* previous = m_name.load(std::memory_order_relaxed);
    publish_name(name);

    // Register with actor registry for name-based lookup. A rename releases
    // the old name, unless another actor has taken it since.
    if (auto shared = m_self_ref.lock()) {
        if (previous && *previous != name) {
            actor_registry::unregister_actor(*previous, this);
        }
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

namespace {

// Raise an atomic maximum; loses only to a concurrent higher value
void raise_to(std::atomic<size_t>& hwm, size_t value) {
    size_t current = hwm.load(std::memory_order_relaxed);
    while (value > current &&
           !hwm.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

} // namespace

void actor::update_high_water_marks(size_t mailbox_size, size_t total) {
    raise_to(m_mailbox_high_water_mark, mailbox_size);
    raise_to(m_total_high_water_mark, total);
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
    const detail::push_result pushed = m_mailbox.push(msg);
    if (pushed == detail::push_result::closed) {
        system::report_dead_letter(name(), msg.get(), m_state.load(), false);
        return;
    }

    // Approximate sizes for metrics and threshold checking
    const size_t mailbox_size = m_mailbox.other_size();
    const size_t total = mailbox_size + m_mailbox.call_size();
    update_high_water_marks(mailbox_size, total);

    // Check threshold: report once per breach, re-armed once the queue is
    // back down to half the threshold.
    if (m_queue_threshold > 0) {
        if (!m_threshold_warning_sent.load(std::memory_order_relaxed)) {
            if (total > m_queue_threshold) {
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
        } else if (total <= m_queue_threshold / 2) {
            m_threshold_warning_sent.store(false, std::memory_order_relaxed);
        }
    }

    // Schedule only on the idle-to-busy transition: while the actor is queued
    // or running, its worker will get to this message without being told.
    if (pushed == detail::push_result::unblocked) {
        system::schedule(this);
    }
}

void actor::enqueue_call_message(std::unique_ptr<message_base> msg) {
    // Don't accept new messages if actor is stopping or stopped
    actor_state current_state = m_state.load();
    if (current_state != actor_state::running) {
        // Report dead letter before dropping
        system::report_dead_letter(name(), msg.get(), current_state, true);
        return;  // Drop message
    }

    // Calls share the mailbox but are delivered next, ahead of every
    // queued ordinary message (INVARIANT 3) - a call must get an answer even
    // from an actor that is behind. Consumed by process_next_message() on the
    // owning thread, so call handlers are serialised with regular handlers
    // (INVARIANT 1). The caller blocks on the request's future until then.
    const detail::push_result pushed = m_mailbox.push(msg);
    if (pushed == detail::push_result::closed) {
        system::report_dead_letter(name(), msg.get(), m_state.load(), true);
        return;  // destroying the request breaks its promise; the caller is not left hanging
    }

    // Update the high water marks of queued calls and of the total
    const size_t call_size = m_mailbox.call_size();
    raise_to(m_call_queue_high_water_mark, call_size);
    raise_to(m_total_high_water_mark, call_size + m_mailbox.other_size());

    // The caller is blocked on the request's future: make sure the actor runs.
    if (pushed == detail::push_result::unblocked) {
        system::schedule(this);
    }
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
    std::cout << "[DISPATCH:" << name() << "] Dispatching message, typeid: " << typeid(*msg).name() << "\n" << std::flush;

    // Debug: print all registered handlers
    std::cout << "[DISPATCH:" << name() << "] Registered handlers count: " << m_handlers.size() << "\n" << std::flush;
    for (const auto& [type_idx, handler] : m_handlers) {
        std::cout << "[DISPATCH:" << name() << "]   - " << type_idx.name() << "\n" << std::flush;
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
        run_stop_hook(stop_hook::on_shutdown);
        // The actor no longer accepts messages: timer ticks from here on would
        // only be dead letters. Includes timers on_shutdown() just scheduled,
        // as stop_actor() does. Owning thread, so m_active_timers is safe.
        system::cancel_actor_timers(this);
        set_state(actor_state::stopping);
        return;
    }

    // Call request (RPC-style call): calls the call handler and sets the promise
    if (kind == message_base::framework_kind::call_request) {
        static_cast<call_request_base*>(msg)->process(this);
        return;
    }

    auto it = m_handlers.find(typeid(*msg));
    try {
        if (it != m_handlers.end()) {
            // Found handler - call it
#ifdef CAS_DEBUG_LOGGING
            std::cout << "[DISPATCH:" << name() << "] Handler found, calling...\n" << std::flush;
#endif
            it->second(msg);
#ifdef CAS_DEBUG_LOGGING
            std::cout << "[DISPATCH:" << name() << "] Handler completed\n" << std::flush;
#endif
        } else {
            // No handler registered for this message type
#ifdef CAS_DEBUG_LOGGING
            std::cout << "[DISPATCH:" << name() << "] No handler found!\n" << std::flush;
#endif
            on_unhandled_message(msg);
        }
    } catch (...) {
        // An inline actor runs on its sender's thread: the sender gets the
        // exception, as documented. Everywhere else it would unwind out of a
        // framework thread and terminate the process (issue 014).
        if (runs_inline()) {
            throw;
        }
        handle_handler_exception(*msg, std::current_exception());
    }
}

bool actor::run_on_start() {
    if (m_started.exchange(true)) {
        return false;
    }
    on_start_scope scope(this);
    on_start();
    return true;
}

exception_action actor::on_handler_exception(const message_base& /*msg*/, std::exception_ptr /*error*/) {
    return exception_action::resume;
}

void actor::handle_handler_exception(const message_base& msg, std::exception_ptr error) {
    exception_action action = exception_action::resume;
    try {
        action = on_handler_exception(msg, error);
    } catch (...) {
        action = exception_action::stop;
    }

    system::report_handler_exception(*this, typeid(msg), msg.message_id, "handler", error, action);

    if (action == exception_action::stop) {
        if (auto shared = m_self_ref.lock()) {
            // This thread executes the actor, so this returns at once and the
            // stop completes after the current handler. Returns false if a
            // stop or system shutdown is already under way - nothing to do.
            stop_config config;
            config.mode = stop_mode::discard;
            config.wait_for_stop = false;
            system::stop_actor(actor_ref(shared), config);
        }
    }
}

void actor::run_stop_hook(stop_hook hook) {
    actor* prev_context = current_actor_context;
    current_actor_context = this;
    try {
        if (hook == stop_hook::on_shutdown) {
            on_shutdown();
        } else {
            on_stop();
        }
    } catch (...) {
        system::report_handler_exception(*this, typeid(void), 0,
                                         hook == stop_hook::on_shutdown ? "on_shutdown" : "on_stop",
                                         std::current_exception(), exception_action::resume);
    }
    current_actor_context = prev_context;
}

void actor::process_next_message() {
    // Delivery order: calls first, then send order per sender (INVARIANT 3).
    // Consumed only here, on the owning thread, so a call handler never runs
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

void actor::report_rejected(const message_base* msg, bool was_call) {
    system::report_dead_letter(name(), msg, m_state.load(), was_call);
}

bool actor::service_stop_request() {
    return system::service_stop(this);
}

void actor::discard_pending_messages() {
    // Destroying a call request breaks its promise: a blocked call() caller gets
    // std::future_error (broken_promise) rather than hanging. A pending
    // system_shutdown is framework control traffic, not a message anyone sent
    // this actor - not a dead letter.
    while (std::unique_ptr<message_base> msg = m_mailbox.pop()) {
        const auto kind = msg->cas_framework_kind();
        if (kind != message_base::framework_kind::shutdown) {
            report_rejected(msg.get(), kind == message_base::framework_kind::call_request);
        }
    }
}

size_t actor::mailbox_high_water_mark() const {
    return m_mailbox_high_water_mark.load();
}

size_t actor::call_queue_high_water_mark() const {
    return m_call_queue_high_water_mark.load();
}

size_t actor::ask_queue_high_water_mark() const {
    return call_queue_high_water_mark();
}

size_t actor::total_high_water_mark() const {
    return m_total_high_water_mark.load();
}

void actor::track_timer(timer_id id) {
    if (id == INVALID_TIMER_ID) {
        return;  // not scheduled: the actor is stopping
    }
    if (m_active_timers.size() >= m_timer_prune_at) {
        system::forget_finished_timers(m_active_timers);
        // Amortised: the next prune comes after as many insertions again
        m_timer_prune_at = std::max<size_t>(64, 2 * m_active_timers.size());
    }
    m_active_timers.insert(id);
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
