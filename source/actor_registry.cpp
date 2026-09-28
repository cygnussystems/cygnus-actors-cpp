#include "cas/actor_registry.h"
#include "cas/actor.h"
#include "cas/actor_ref.h"
#include "cas/actor_ref_impl.h"

namespace cas {

actor_registry& actor_registry::instance() {
    static actor_registry registry;
    return registry;
}

void actor_registry::register_actor(const std::string& name, std::shared_ptr<actor> actor_ptr) {
    auto& inst = instance();
    std::lock_guard<std::mutex> lock(inst.m_mutex);
    inst.m_actors[name] = actor_ptr;
}

void actor_registry::unregister_actor(const std::string& name) {
    auto& inst = instance();
    std::lock_guard<std::mutex> lock(inst.m_mutex);
    inst.m_actors.erase(name);
}

void actor_registry::unregister_actor(const std::string& name, const actor* expected) {
    auto& inst = instance();
    std::lock_guard<std::mutex> lock(inst.m_mutex);
    auto it = inst.m_actors.find(name);
    if (it != inst.m_actors.end()) {
        auto current = it->second.lock();
        if (!current || current.get() == expected) {
            inst.m_actors.erase(it);
        }
    }
}

actor_ref actor_registry::get(const std::string& name) {
    auto& inst = instance();
    std::lock_guard<std::mutex> lock(inst.m_mutex);

    auto it = inst.m_actors.find(name);
    if (it != inst.m_actors.end()) {
        if (auto actor_ptr = it->second.lock()) {
            return actor_ref(actor_ptr);
        }
        inst.m_actors.erase(it);  // the actor is gone
    }

    // Return empty actor_ref if not found
    return actor_ref();
}

bool actor_registry::exists(const std::string& name) {
    auto& inst = instance();
    std::lock_guard<std::mutex> lock(inst.m_mutex);
    auto it = inst.m_actors.find(name);
    return it != inst.m_actors.end() && !it->second.expired();
}

void actor_registry::clear() {
    auto& inst = instance();
    std::lock_guard<std::mutex> lock(inst.m_mutex);
    inst.m_actors.clear();
}

size_t actor_registry::count() {
    auto& inst = instance();
    std::lock_guard<std::mutex> lock(inst.m_mutex);
    std::erase_if(inst.m_actors, [](const auto& entry) { return entry.second.expired(); });
    return inst.m_actors.size();
}

} // namespace cas
