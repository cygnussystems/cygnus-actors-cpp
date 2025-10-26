#ifndef CAS_ACTOR_REGISTRY_H
#define CAS_ACTOR_REGISTRY_H

#include <string>
#include <unordered_map>
#include <mutex>
#include <memory>

namespace cas {

// Forward declarations
class actor;
class actor_ref;

// DNS-like registry for looking up actors by name
// Allows actors to discover each other without hard references
class actor_registry {
private:
    // Map from actor name to actor
    std::unordered_map<std::string, std::shared_ptr<actor>> m_actors;
    mutable std::mutex m_mutex;

    // Singleton instance
    static actor_registry& instance();

    // Private constructor for singleton
    actor_registry() = default;

public:
    // Non-copyable
    actor_registry(const actor_registry&) = delete;
    actor_registry& operator=(const actor_registry&) = delete;

    // Register an actor with a name
    static void register_actor(const std::string& name, std::shared_ptr<actor> actor_ptr);

    // Unregister an actor
    static void unregister_actor(const std::string& name);

    // Look up an actor by name
    // Returns empty actor_ref if not found
    static actor_ref get(const std::string& name);

    // Check if an actor with this name exists
    static bool exists(const std::string& name);

    // Clear all registered actors (useful for testing)
    static void clear();

    // Get count of registered actors
    static size_t count();
};

} // namespace cas

#endif // CAS_ACTOR_REGISTRY_H
