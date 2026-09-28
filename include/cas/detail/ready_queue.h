// A worker's queue of actors ready to run. Same algorithm as cas::detail::mailbox
// (itself adapted from CAF's lifo_inbox, BSD-3-Clause, (c) 2011-2024 Dominik
// Charousset - see include/cas/detail/mailbox.h for the full notice), without
// the blocked/closed states.
//
// Modifications (c) 2026 cygnussystems, MIT License.

#ifndef CAS_DETAIL_READY_QUEUE_H
#define CAS_DETAIL_READY_QUEUE_H

#include <atomic>
#include <memory>

namespace cas {

class actor;

namespace detail {

/// Actors ready to run on one worker: any number of producers, one consumer
/// (the worker).
///
/// FIFO in push order across all producers: pushes are totally ordered by the
/// CAS on one word, and the consumer reverses the stack it takes. Fairness
/// depends on that - an actor that re-queues itself after using its turn goes
/// behind every actor queued before it. (moodycamel::ConcurrentQueue, used here
/// first, is not fair across producers: a consumer keeps draining one
/// producer's sub-queue for a quota of items, so a busy actor re-queued by the
/// worker starved actors queued from other threads.)
///
/// Intrusive: links through actor's ready-queue link, so no allocation. That
/// requires an actor to be in at most one ready queue at most once, which the
/// scheduler guarantees: an actor is pushed only by whoever wins its
/// idle-to-busy transition, or by the worker that is running it. While queued,
/// the actor holds a strong reference to itself (handed back by pop()), so it
/// cannot be destroyed in the queue.
// The alignas(64) below is deliberate (producer-side atomics on their own
// cache line); MSVC /W4 warns (C4324) that the class is padded for it.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4324)
#endif
class ready_queue {
public:
    ready_queue() noexcept = default;
    ~ready_queue();

    ready_queue(const ready_queue&) = delete;
    ready_queue& operator=(const ready_queue&) = delete;

    /// Any thread. `self` must own `a` and a must not already be queued.
    void push(std::shared_ptr<actor> self) noexcept;

    /// Worker only. Next actor in FIFO order, or nullptr.
    std::shared_ptr<actor> pop() noexcept;

    /// Worker only; used when deciding whether to park.
    bool maybe_nonempty() const noexcept;

    /// Worker only (or when no worker runs). Drop every queued entry.
    void clear() noexcept;

private:
    bool fetch_more() noexcept;

    // Owner-only FIFO cache, oldest first
    actor* m_head = nullptr;
    actor* m_tail = nullptr;

    // Shared: newest-first stack of pushed actors
    alignas(64) std::atomic<actor*> m_stack{nullptr};
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace detail
} // namespace cas

#endif // CAS_DETAIL_READY_QUEUE_H
