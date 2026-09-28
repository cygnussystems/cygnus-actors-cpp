#ifndef CAS_DETAIL_SELECTIVE_STASH_H
#define CAS_DETAIL_SELECTIVE_STASH_H

#include <atomic>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <typeindex>
#include <unordered_set>

namespace cas {

class actor;
struct message_base;

namespace detail {

/// Selective receive for stateful_actor and stateful_fast_actor: the logic
/// lives here once, and both classes forward to it.
///
/// Messages arrive through the actor's normal mailbox (so the actor is
/// scheduled like any other). process_next() takes them out in delivery order:
///  - framework messages (calls, system shutdown) are handled at once - they
///    never wait behind the filter;
///  - an ordinary message whose type is accepted in the current state is
///    handled, oldest first;
///  - anything else is held here until a state change accepts it.
///
/// Owner-only: every member except the counters is touched only by the thread
/// executing the actor, so there are no locks. Accept/reject calls must come
/// from that actor's own on_start() or handlers. The counters are atomic so
/// has_messages() and queue_size() can be read from other threads (the
/// shutdown drain does).
class selective_stash {
public:
    // --- filter (owner only) ---
    void accept(std::type_index type);
    void reject(std::type_index type);
    void accept_all();
    void reject_all();

    // --- used by the actor classes (owner only unless noted) ---

    /// Handle the next message for `self`, if there is one it may handle now.
    void process_next(actor& self);

    /// Anything the actor can do right now? (any thread; approximate)
    bool has_work(const actor& self) const;

    /// Messages queued or held (any thread; approximate)
    size_t queued(const actor& self) const;

    /// Held messages, for the shutdown log and metrics (any thread)
    size_t held() const noexcept { return m_held_count.load(std::memory_order_relaxed); }

    /// Drop every held message, reporting each as a dead letter (owner only,
    /// or when no thread executes the actor).
    void drop_held(actor& self);

private:
    // Dispatch on the calling (owning) thread, as actor::process_next_message() does
    static void run(actor& self, std::unique_ptr<message_base> msg);

    bool accepts(const message_base& msg) const;
    void recount_acceptable();

    std::deque<std::unique_ptr<message_base>> m_held;  // oldest first
    std::unordered_set<std::type_index> m_accepted;
    bool m_accept_all = true;

    std::atomic<size_t> m_held_count{0};        // all held messages
    std::atomic<size_t> m_acceptable_count{0};  // held messages accepted now
};

} // namespace detail
} // namespace cas

#endif // CAS_DETAIL_SELECTIVE_STASH_H
