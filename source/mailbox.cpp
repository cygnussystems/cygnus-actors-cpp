// Adapted from CAF, the C++ Actor Framework - see include/cas/detail/mailbox.h
// for the copyright notice, license conditions and disclaimer.
// Copyright (c) 2011-2024, Dominik Charousset. All rights reserved.
// Modifications (c) 2026 cygnussystems, MIT License.

#include "cas/detail/mailbox.h"
#include "cas/message_base.h"

#include <cstdint>

namespace cas::detail {

mailbox::mailbox() noexcept {
    m_stack.store(empty_tag(), std::memory_order_relaxed);
}

mailbox::~mailbox() {
    // Owner is gone, so no concurrent access: free whatever is left.
    auto free_list = [](message_base* node) {
        while (node != nullptr) {
            message_base* next = node->m_next;
            delete node;
            node = next;
        }
    };
    message_base* stack = m_stack.load(std::memory_order_acquire);
    free_list(is_tag(stack) ? nullptr : stack);
    free_list(m_calls.head);
    free_list(m_others.head);
}

message_base* mailbox::blocked_tag() const noexcept {
    // Never dereferenced: this object's own address marks "owner blocked".
    return reinterpret_cast<message_base*>(const_cast<mailbox*>(this));
}

message_base* mailbox::closed_tag() const noexcept {
    // Never dereferenced: this object's address + 1 marks "closed".
    return reinterpret_cast<message_base*>(reinterpret_cast<std::uintptr_t>(this) + 1);
}

bool mailbox::is_tag(message_base* p) const noexcept {
    return p == empty_tag() || p == blocked_tag() || p == closed_tag();
}

bool mailbox::is_call(const message_base& msg) noexcept {
    return msg.cas_framework_kind() == message_base::framework_kind::call_request;
}

std::atomic<size_t>& mailbox::count_for(const message_base& msg) noexcept {
    return is_call(msg) ? m_call_count : m_other_count;
}

push_result mailbox::push(std::unique_ptr<message_base>& msg) noexcept {
    message_base* node = msg.get();
    std::atomic<size_t>& count = count_for(*node);

    // Count before publishing: the consumer can only decrement after it has
    // taken the node, so the count never drops below zero.
    count.fetch_add(1, std::memory_order_relaxed);

    message_base* head = m_stack.load(std::memory_order_relaxed);
    const message_base* const closed = closed_tag();
    const message_base* const blocked = blocked_tag();
    while (head != closed) {
        // A tag is never part of a non-empty list.
        node->m_next = (head != blocked) ? head : nullptr;
        if (m_stack.compare_exchange_weak(head, node,
                                          std::memory_order_acq_rel,
                                          std::memory_order_relaxed)) {
            msg.release();  // now owned by the mailbox
            return head == blocked ? push_result::unblocked : push_result::success;
        }
        // head was reloaded by the failed CAS; retry
    }

    count.fetch_sub(1, std::memory_order_relaxed);
    return push_result::closed;  // caller keeps msg
}

message_base* mailbox::take_head(message_base* new_head) noexcept {
    message_base* head = m_stack.load(std::memory_order_relaxed);
    while (head != new_head) {
        if (m_stack.compare_exchange_weak(head, new_head,
                                          std::memory_order_acq_rel,
                                          std::memory_order_relaxed)) {
            return is_tag(head) ? nullptr : head;
        }
    }
    return nullptr;
}

bool mailbox::fetch_more() noexcept {
    message_base* node = take_head(empty_tag());
    if (node == nullptr) {
        return false;
    }

    // The stack is newest-first. Reversing it gives push order; calls and other
    // messages are split into their own caches as we go.
    message_base* ask_first = nullptr;
    message_base* ask_last = nullptr;
    message_base* other_first = nullptr;
    message_base* other_last = nullptr;
    while (node != nullptr) {
        message_base* next = node->m_next;
        if (is_call(*node)) {
            node->m_next = ask_first;
            if (ask_first == nullptr) ask_last = node;
            ask_first = node;
        } else {
            node->m_next = other_first;
            if (other_first == nullptr) other_last = node;
            other_first = node;
        }
        node = next;
    }

    auto append = [](fifo& q, message_base* first, message_base* last) {
        if (first == nullptr) return;
        if (q.tail == nullptr) {
            q.head = first;
        } else {
            q.tail->m_next = first;
        }
        q.tail = last;
    };
    append(m_calls, ask_first, ask_last);
    append(m_others, other_first, other_last);
    return true;
}

std::unique_ptr<message_base> mailbox::pop() noexcept {
    // A call may be on the stack while ordinary messages are already cached
    // (a whole backlog is taken in one exchange). Its count is published before
    // the push, so a queued-but-uncached call shows up here and we fetch it
    // before serving another ordinary message.
    if (m_calls.head == nullptr && m_call_count.load(std::memory_order_relaxed) > 0) {
        fetch_more();
    }
    if (m_calls.head == nullptr && m_others.head == nullptr && !fetch_more()) {
        return nullptr;
    }

    fifo& q = (m_calls.head != nullptr) ? m_calls : m_others;
    message_base* node = q.head;
    q.head = node->m_next;
    if (q.head == nullptr) q.tail = nullptr;
    node->m_next = nullptr;
    count_for(*node).fetch_sub(1, std::memory_order_relaxed);
    return std::unique_ptr<message_base>(node);
}

bool mailbox::try_block() noexcept {
    if (m_calls.head != nullptr || m_others.head != nullptr) {
        return false;
    }
    message_base* expected = empty_tag();
    return m_stack.compare_exchange_strong(expected, blocked_tag(),
                                           std::memory_order_seq_cst);
}

bool mailbox::try_unblock() noexcept {
    message_base* expected = blocked_tag();
    return m_stack.compare_exchange_strong(expected, empty_tag(),
                                           std::memory_order_seq_cst);
}

size_t mailbox::close(const std::function<void(message_base&, bool is_call)>& on_dropped) {
    // Close first, so no push can slip in behind the drain.
    message_base* stack = take_head(closed_tag());

    size_t dropped = 0;
    auto drain = [&](message_base* node) {
        while (node != nullptr) {
            message_base* next = node->m_next;
            count_for(*node).fetch_sub(1, std::memory_order_relaxed);
            if (on_dropped) on_dropped(*node, is_call(*node));
            delete node;
            ++dropped;
            node = next;
        }
    };
    drain(m_calls.head);    // cached messages, oldest first
    m_calls = {};
    drain(m_others.head);
    m_others = {};
    drain(stack);   // then anything pushed since (newest first)
    return dropped;
}

size_t mailbox::size() const noexcept {
    return call_size() + other_size();
}

size_t mailbox::call_size() const noexcept {
    return m_call_count.load(std::memory_order_relaxed);
}

size_t mailbox::other_size() const noexcept {
    return m_other_count.load(std::memory_order_relaxed);
}

bool mailbox::closed() const noexcept {
    return m_stack.load(std::memory_order_acquire) == closed_tag();
}

bool mailbox::blocked() const noexcept {
    return m_stack.load(std::memory_order_acquire) == blocked_tag();
}

} // namespace cas::detail
