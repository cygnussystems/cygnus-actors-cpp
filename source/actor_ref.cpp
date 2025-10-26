#include "cas/actor_ref.h"
#include "cas/actor.h"
#include <stdexcept>

namespace cas {

actor_ref::actor_ref(std::shared_ptr<actor> ptr)
    : m_actor_ptr(ptr) {
}

bool actor_ref::is_valid() const {
    return m_actor_ptr != nullptr;
}

actor_ref::operator bool() const {
    return is_valid();
}

std::string actor_ref::name() const {
    if (!m_actor_ptr) return "";
    return m_actor_ptr->name();
}

std::shared_ptr<actor> actor_ref::get_actor() const {
    return m_actor_ptr;
}

bool actor_ref::operator==(const actor_ref& other) const {
    return m_actor_ptr == other.m_actor_ptr;
}

bool actor_ref::operator!=(const actor_ref& other) const {
    return !(*this == other);
}

// Template implementations would go in header or be explicitly instantiated
// For now, leaving as header-only templates

} // namespace cas
