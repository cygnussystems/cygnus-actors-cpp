// Same algorithm as source/mailbox.cpp (adapted from CAF, BSD-3-Clause,
// (c) 2011-2024 Dominik Charousset - see include/cas/detail/mailbox.h).
// Modifications (c) 2026 cygnussystems, MIT License.

#include "cas/detail/ready_queue.h"
#include "cas/actor.h"

namespace cas::detail {

ready_queue::~ready_queue() {
    clear();
}

void ready_queue::push(std::shared_ptr<actor> self) noexcept {
    actor* node = self.get();
    // Published by the release CAS below; only the consumer reads it back.
    node->m_ready_self = std::move(self);

    actor* head = m_stack.load(std::memory_order_relaxed);
    do {
        node->m_ready_next = head;
    } while (!m_stack.compare_exchange_weak(head, node,
                                            std::memory_order_acq_rel,
                                            std::memory_order_relaxed));
}

bool ready_queue::fetch_more() noexcept {
    actor* node = m_stack.exchange(nullptr, std::memory_order_acq_rel);
    if (node == nullptr) {
        return false;
    }
    // Newest-first stack -> oldest-first list, appended to the cache.
    actor* first = nullptr;
    actor* const last = node;
    while (node != nullptr) {
        actor* next = node->m_ready_next;
        node->m_ready_next = first;
        first = node;
        node = next;
    }
    if (m_tail == nullptr) {
        m_head = first;
    } else {
        m_tail->m_ready_next = first;
    }
    m_tail = last;
    return true;
}

std::shared_ptr<actor> ready_queue::pop() noexcept {
    if (m_head == nullptr && !fetch_more()) {
        return nullptr;
    }
    actor* node = m_head;
    m_head = node->m_ready_next;
    if (m_head == nullptr) m_tail = nullptr;
    node->m_ready_next = nullptr;
    return std::move(node->m_ready_self);
}

bool ready_queue::maybe_nonempty() const noexcept {
    return m_head != nullptr || m_stack.load(std::memory_order_relaxed) != nullptr;
}

void ready_queue::clear() noexcept {
    while (std::shared_ptr<actor> dropped = pop()) {
        // Releasing the self-reference; the actor may be destroyed here.
    }
}

} // namespace cas::detail
