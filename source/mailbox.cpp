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
    free_list(m_head);
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

bool mailbox::is_ask(const message_base& msg) noexcept {
    return msg.cas_framework_kind() == message_base::framework_kind::ask_request;
}

std::atomic<size_t>& mailbox::count_for(const message_base& msg) noexcept {
    return is_ask(msg) ? m_ask_count : m_other_count;
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

    // The stack is newest-first; reversing it gives push (delivery) order.
    message_base* first = nullptr;
    message_base* const last = node;  // the newest becomes the new tail
    while (node != nullptr) {
        message_base* next = node->m_next;
        node->m_next = first;
        first = node;
        node = next;
    }

    if (m_tail == nullptr) {
        m_head = first;
    } else {
        m_tail->m_next = first;
    }
    m_tail = last;
    return true;
}

std::unique_ptr<message_base> mailbox::pop() noexcept {
    if (m_head == nullptr && !fetch_more()) {
        return nullptr;
    }
    message_base* node = m_head;
    m_head = node->m_next;
    if (m_head == nullptr) m_tail = nullptr;
    node->m_next = nullptr;
    count_for(*node).fetch_sub(1, std::memory_order_relaxed);
    return std::unique_ptr<message_base>(node);
}

bool mailbox::try_block() noexcept {
    if (m_head != nullptr) {
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

size_t mailbox::close(const std::function<void(message_base&, bool is_ask)>& on_dropped) {
    // Close first, so no push can slip in behind the drain.
    message_base* stack = take_head(closed_tag());

    size_t dropped = 0;
    auto drain = [&](message_base* node) {
        while (node != nullptr) {
            message_base* next = node->m_next;
            count_for(*node).fetch_sub(1, std::memory_order_relaxed);
            if (on_dropped) on_dropped(*node, is_ask(*node));
            delete node;
            ++dropped;
            node = next;
        }
    };
    drain(m_head);  // cached messages, oldest first
    m_head = m_tail = nullptr;
    drain(stack);   // then anything pushed since (newest first)
    return dropped;
}

size_t mailbox::size() const noexcept {
    return ask_size() + other_size();
}

size_t mailbox::ask_size() const noexcept {
    return m_ask_count.load(std::memory_order_relaxed);
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
