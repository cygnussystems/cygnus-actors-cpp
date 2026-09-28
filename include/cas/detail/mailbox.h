// Adapted from CAF, the C++ Actor Framework
// (libcaf_core/caf/intrusive/lifo_inbox.hpp and caf/detail/default_mailbox.*,
// https://github.com/actor-framework/actor-framework, commit 42882ae).
//
// Copyright (c) 2011-2024, Dominik Charousset
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
//   list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
//
// * Neither the name of the copyright holder nor the names of its
//   contributors may be used to endorse or promote products derived from
//   this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Modifications for Cygnus (c) 2026 cygnussystems, MIT License: a single class
// over cas::message_base; calls take CAF's "urgent" role and are served
// ahead of other messages, including ones already taken from the stack; atomic
// counts so size and metrics can be read from any thread.

#ifndef CAS_DETAIL_MAILBOX_H
#define CAS_DETAIL_MAILBOX_H

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>

namespace cas {

struct message_base;

namespace detail {

/// Result of pushing to a mailbox.
enum class push_result {
    success,    ///< Queued; the owner is already running or scheduled.
    unblocked,  ///< Queued; the owner was idle (blocked) and must be scheduled.
    closed      ///< Rejected; the mailbox is closed. The caller keeps the message.
};

/// An actor's mailbox: any number of producers, exactly one consumer.
///
/// Producers push onto an intrusive lock-free LIFO stack - one atomic word,
/// linked through message_base's next pointer, no node allocation. The
/// consumer (the thread executing the actor) takes the whole stack with one
/// exchange and reverses it into two private FIFO caches: calls and
/// everything else.
///
/// Ordering: pushes are totally ordered by the CAS on the stack word, and each
/// cache delivers in that order. So among ordinary messages - and among calls -
/// if one push happens-before another (same actor, same thread), the first is
/// delivered first (INVARIANT 3). Calls are the exception across the two: an
/// call is delivered next, ahead of every ordinary message still queued, even
/// ones its own sender pushed earlier. pop() checks for newly pushed calls
/// before serving each ordinary message, so a call does not wait behind a
/// batch already taken from the stack.
///
/// The stack word also carries two states, as sentinel values that are never
/// dereferenced:
///  - blocked: the owner has nothing to do. The next push flips it back and
///    returns push_result::unblocked, which is the signal to schedule it.
///  - closed: the owner has stopped. Pushes fail and hand the message back.
///
/// Not subject to ABA: producers only push, and the consumer never pops a
/// single node by CAS - it takes the entire stack (see take_head()).
// The alignas(64) below is deliberate (producer-side atomics on their own
// cache line); MSVC /W4 warns (C4324) that the class is padded for it.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4324)
#endif
class mailbox {
public:
    mailbox() noexcept;
    ~mailbox();

    mailbox(const mailbox&) = delete;
    mailbox& operator=(const mailbox&) = delete;

    // --- any thread ---------------------------------------------------------

    /// Queues msg. On success or unblocked, takes ownership. On closed, msg is
    /// left untouched and the caller still owns it.
    push_result push(std::unique_ptr<message_base>& msg) noexcept;

    /// Approximate counts (exact when no push or pop is in progress).
    size_t size() const noexcept;
    size_t call_size() const noexcept;    ///< queued calls
    size_t other_size() const noexcept;  ///< everything else

    bool closed() const noexcept;
    bool blocked() const noexcept;

    // --- owner (consumer) only ----------------------------------------------

    /// Next message: a call if any is queued, else the oldest ordinary message;
    /// nullptr if there is none.
    std::unique_ptr<message_base> pop() noexcept;

    /// Transition to blocked if there is nothing left to consume. Returns false
    /// if messages are cached or were pushed meanwhile.
    bool try_block() noexcept;

    /// Transition from blocked back to empty. Returns false if not blocked
    /// (e.g. a push already unblocked it).
    bool try_unblock() noexcept;

    /// Close the mailbox: later pushes fail. Every message still queued is
    /// handed to on_dropped (which does not take ownership; the message is
    /// deleted afterwards). Returns how many there were.
    size_t close(const std::function<void(message_base&, bool is_call)>& on_dropped);

private:
    static bool is_call(const message_base& msg) noexcept;
    std::atomic<size_t>& count_for(const message_base& msg) noexcept;
    message_base* take_head(message_base* new_head) noexcept;
    bool fetch_more() noexcept;

    message_base* empty_tag() const noexcept { return nullptr; }
    message_base* blocked_tag() const noexcept;
    message_base* closed_tag() const noexcept;
    bool is_tag(message_base* p) const noexcept;

    struct fifo {
        message_base* head = nullptr;
        message_base* tail = nullptr;
    };

    // Owner-only FIFO caches, oldest first (CAF's urgent_queue_/normal_queue_)
    fifo m_calls;
    fifo m_others;

    // Shared by all producers: the LIFO stack and the counts they update.
    // Kept together on their own cache line, away from the owner's cache.
    alignas(64) std::atomic<message_base*> m_stack;
    std::atomic<size_t> m_call_count{0};
    std::atomic<size_t> m_other_count{0};
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace detail
} // namespace cas

#endif // CAS_DETAIL_MAILBOX_H
