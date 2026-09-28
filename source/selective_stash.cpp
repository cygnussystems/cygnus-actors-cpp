#include "cas/detail/selective_stash.h"
#include "cas/actor.h"
#include "cas/message_base.h"
#include "cas/system.h"

namespace cas::detail {

void selective_stash::run(actor& self, std::unique_ptr<message_base> msg) {
    actor* previous = current_actor_context;
    current_actor_context = &self;
    self.dispatch_message(msg.get());
    current_actor_context = previous;
}

void selective_stash::accept(std::type_index type) {
    m_accept_all = false;
    m_accepted.insert(type);
    recount_acceptable();
}

void selective_stash::reject(std::type_index type) {
    // In accept-all mode there is no list to remove from: rejecting is a no-op,
    // as it always was. To accept everything but X, reject_all() and then
    // accept the types wanted.
    if (m_accept_all) {
        return;
    }
    m_accepted.erase(type);
    recount_acceptable();
}

void selective_stash::accept_all() {
    m_accept_all = true;
    m_accepted.clear();
    recount_acceptable();
}

void selective_stash::reject_all() {
    m_accept_all = false;
    m_accepted.clear();
    recount_acceptable();
}

bool selective_stash::accepts(const message_base& msg) const {
    return m_accept_all || m_accepted.count(std::type_index(typeid(msg))) != 0;
}

void selective_stash::recount_acceptable() {
    size_t n = 0;
    for (const auto& msg : m_held) {
        if (accepts(*msg)) ++n;
    }
    m_acceptable_count.store(n, std::memory_order_relaxed);
}

void selective_stash::process_next(actor& self) {
    // Take from the mailbox in delivery order. A framework message is handled
    // at once; ordinary messages go behind the held ones, keeping their order.
    while (std::unique_ptr<message_base> msg = self.m_mailbox.pop()) {
        if (msg->cas_framework_kind() != message_base::framework_kind::user) {
            run(self, std::move(msg));
            return;
        }
        if (accepts(*msg)) m_acceptable_count.fetch_add(1, std::memory_order_relaxed);
        m_held.push_back(std::move(msg));
        m_held_count.fetch_add(1, std::memory_order_relaxed);
    }

    // Oldest held message the current state accepts. Held messages are few in
    // practice; this is a linear scan by design (see the class comment).
    for (auto it = m_held.begin(); it != m_held.end(); ++it) {
        if (accepts(**it)) {
            std::unique_ptr<message_base> next = std::move(*it);
            m_held.erase(it);
            m_held_count.fetch_sub(1, std::memory_order_relaxed);
            m_acceptable_count.fetch_sub(1, std::memory_order_relaxed);
            // The handler may change the filter; recount_acceptable() in
            // accept()/reject() keeps m_acceptable_count right.
            run(self, std::move(next));
            return;
        }
    }
}

bool selective_stash::has_work(const actor& self) const {
    return self.m_mailbox.size() > 0 ||
           m_acceptable_count.load(std::memory_order_relaxed) > 0;
}

size_t selective_stash::queued(const actor& self) const {
    return self.m_mailbox.size() + held();
}

void selective_stash::drop_held(actor& self) {
    for (auto& msg : m_held) {
        system::report_dead_letter(self.name(), msg.get(), self.get_state(), false);
    }
    m_held.clear();
    m_held_count.store(0, std::memory_order_relaxed);
    m_acceptable_count.store(0, std::memory_order_relaxed);
}

} // namespace cas::detail
