#ifndef CAS_SYSTEM_H
#define CAS_SYSTEM_H

#include <memory>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <queue>
#include <condition_variable>
#include "timer.h"
#include "timer_manager.h"
#include "external/concurrentqueue.h"

namespace cas {

// Forward declarations
class actor;
class actor_ref;
struct scheduled_timer;

// Threading model for actors
enum class threading_model {
    pooled,    // Runs on shared thread pool (default)
    dedicated  // Runs on its own dedicated thread
};

// Configuration for the actor system
struct system_config {
    size_t thread_pool_size = 0;      // 0 = auto (use hardware concurrency)
    size_t ask_thread_pool_size = 4;  // Dedicated threads for ask (RPC) requests
    size_t queue_threshold = 1000;    // Warn if total queue size exceeds this (0 = disabled)
};

// Configuration for shutdown behavior
struct shutdown_config {
    std::chrono::milliseconds drain_timeout{5000};  // Max time to wait for queues to drain
    std::chrono::milliseconds check_interval{10};   // How often to check if queues are empty
};

// The actor system/runtime
// Manages actor lifecycle, threading, and message processing
class system {
private:
    // Configuration
    system_config m_config;

    // Actors organized by thread affinity
    // Each vector contains actors assigned to that thread
    std::vector<std::vector<std::shared_ptr<actor>>> m_actors_per_thread;
    std::mutex m_actors_mutex;

    // Thread pool for pooled actors (actor affinity - each actor pinned to one thread)
    std::vector<std::thread> m_thread_pool;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_shutdown_requested{false};

    // Fast actors with dedicated threads
    std::vector<std::shared_ptr<actor>> m_fast_actors;
    std::mutex m_fast_actors_mutex;

    // Next thread to assign actor to (round-robin assignment)
    std::atomic<size_t> m_next_thread_assignment{0};

    // Message ID counter (globally unique across all messages)
    std::atomic<uint64_t> m_next_message_id{1};  // Start at 1, 0 means "no ID"

    // Instance ID counter (globally unique across all actors)
    std::atomic<size_t> m_next_instance_id{1};  // Start at 1, 0 means "unassigned"

    // Shutdown log - warnings/errors during shutdown
    std::vector<std::string> m_shutdown_log;
    std::mutex m_shutdown_log_mutex;

    // Timer management
    timer_manager m_timer_manager;

    // Ask (RPC) request handling
    struct ask_request_envelope {
        actor* target;  // Target actor
        std::unique_ptr<message_base> request;  // The ask request message
    };
    moodycamel::ConcurrentQueue<ask_request_envelope> m_global_ask_queue;
    std::vector<std::thread> m_ask_thread_pool;

    // Singleton instance
    static system& instance();

    // Private constructor for singleton
    system();

    // Worker thread function for thread pool
    void worker_thread(size_t thread_id);

    // Ask worker thread function
    void ask_worker_thread();

public:
    // Non-copyable
    system(const system&) = delete;
    system& operator=(const system&) = delete;

    // Destructor
    ~system();

    // Initialize the system with configuration (optional - auto-initializes on first use)
    static void init(const system_config& config = system_config{});

    // Set configuration before starting
    static void configure(const system_config& config);

    // Create an actor and register it with the system
    // Returns an actor_ref for sending messages
    template<typename ActorType, typename... Args>
    static actor_ref create(Args&&... args);

    // Start the system (starts all actors and begins message processing)
    static void start();

    // Request shutdown of the system
    // Optional config for drain timeout and check interval
    static void shutdown(const shutdown_config& config = shutdown_config{});

    // Wait for the system to complete shutdown
    static void wait_for_shutdown();

    // Get shutdown log (warnings/errors that occurred during shutdown)
    static std::vector<std::string> get_shutdown_log();

    // Reset the system to initial state (for testing)
    // This must be called after shutdown completes
    static void reset();

    // Check if system is running
    static bool is_running();

    // Get number of actors in the system
    static size_t actor_count();

    // Internal: register an actor with the system
    static void register_actor(std::shared_ptr<actor> actor_ptr);

    // Internal: get reference to system instance
    static system* get_instance();

    // Internal: get next unique message ID
    static uint64_t next_message_id();

    // Internal: Timer management (called by actor::schedule_once/periodic)
    static timer_id schedule_timer(actor* target, std::unique_ptr<message_base> msg,
                                   std::function<std::unique_ptr<message_base>()> copy_func,
                                   std::chrono::milliseconds delay,
                                   std::chrono::milliseconds interval = std::chrono::milliseconds(0));

    // Internal: Cancel a timer (called by actor::cancel_timer)
    static void cancel_timer_internal(timer_id id);

    // Internal: Cancel all timers for an actor (called on actor stop)
    static void cancel_actor_timers(actor* target);

    // Internal: Enqueue ask request to global queue (called by actor::enqueue_ask_message)
    static void enqueue_global_ask(actor* target, std::unique_ptr<message_base> request);
};

// Template implementations (must be in header)

template<typename ActorType, typename... Args>
actor_ref system::create(Args&&... args) {
    // Create the actor
    auto actor_ptr = std::make_shared<ActorType>(std::forward<Args>(args)...);

    // Register with system (assigns thread affinity and instance ID)
    register_actor(actor_ptr);

    // Register with actor registry if it has a name
    // (will be set in on_start, so registry lookup happens after start)

    // Return reference to the actor
    return actor_ref(actor_ptr);
}

} // namespace cas

#endif // CAS_SYSTEM_H
