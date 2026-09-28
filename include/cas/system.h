#ifndef CAS_SYSTEM_H
#define CAS_SYSTEM_H

#include <memory>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <condition_variable>
#include <functional>
#include <typeindex>
#include <exception>
#include <set>
#include "timer.h"
#include "timer_manager.h"
#include "external/concurrentqueue.h"
#include "detail/ready_queue.h"

namespace cas {

// Forward declarations
class actor;
class actor_ref;
struct scheduled_timer;
struct message_base;

// Threading model for actors
enum class threading_model {
    pooled,    // Runs on shared thread pool (default)
    dedicated  // Runs on its own dedicated thread
};

// Forward declarations (defined in actor.h)
enum class actor_state;
enum class exception_action;

// Dead letter statistics - always collected (negligible overhead)
struct dead_letter_stats {
    size_t dropped_tell = 0;       // Fire-and-forget messages dropped
    size_t dropped_ask = 0;        // call() requests dropped (no response sent); 1.0 name kept
    size_t dropped_to_invalid = 0; // tell()/call(timeout) on an invalid actor_ref
};

// Information about a dead letter (passed to callback handler)
struct dead_letter_info {
    std::string target_actor_name;   // Name of the target actor
    std::type_index message_type;    // Type of the dropped message
    uint64_t message_id;             // Unique message ID
    actor_state target_state;        // State of actor when message was dropped

    dead_letter_info(std::string name, std::type_index type, uint64_t id, actor_state state)
        : target_actor_name(std::move(name)), message_type(type), message_id(id), target_state(state) {}
};

// Dead letter handler callback type
using dead_letter_handler = std::function<void(const dead_letter_info&)>;

// Information about a queue threshold breach (passed to callback handler)
struct queue_threshold_info {
    std::string actor_name;  // Name of the actor whose queue breached
    size_t queue_size;       // Approximate total queued messages at breach
    size_t threshold;        // The configured threshold that was exceeded

    queue_threshold_info(std::string name, size_t size, size_t limit)
        : actor_name(std::move(name)), queue_size(size), threshold(limit) {}
};

// Queue threshold handler callback type
// Called once per breach, from the thread that enqueued the message.
// Keep the handler cheap and non-blocking - it runs on the send path.
using queue_threshold_handler = std::function<void(const queue_threshold_info&)>;

/// An exception thrown by an actor's message handler or stop hook, caught by
/// the framework (passed to the handler_exception_handler callback)
struct handler_exception_info {
    std::string actor_name;
    std::type_index message_type;  ///< the message being handled; typeid(void) for a hook
    uint64_t message_id;           ///< 0 for a hook
    std::string source;            ///< "handler", "on_shutdown" or "on_stop"
    std::string what;              ///< what() of a std::exception, else "unknown exception"
    std::exception_ptr error;      ///< the exception itself, for rethrow-and-inspect
    exception_action action;       ///< what the actor does next (resume for hooks)

    handler_exception_info(std::string name, std::type_index type, uint64_t id,
                           std::string src, std::string text, std::exception_ptr err,
                           exception_action next)
        : actor_name(std::move(name)), message_type(type), message_id(id), source(std::move(src)),
          what(std::move(text)), error(std::move(err)), action(next) {}
};

/// Callback for handler exceptions. Runs on the actor's executing thread,
/// right after the failed handler; keep it cheap and non-blocking.
using handler_exception_handler = std::function<void(const handler_exception_info&)>;

// Configuration for the actor system
struct system_config {
    size_t thread_pool_size = 0;      // 0 = auto (use hardware concurrency)
    size_t queue_threshold = 1000;    // Warn if total queue size exceeds this (0 = disabled)
    bool log_dead_letters = false;    // Log dropped messages to stderr (for debugging)

    // Most messages a pooled actor handles in one turn before its worker moves
    // on to other ready actors (it is re-queued if it still has messages).
    // Higher favours throughput of a busy actor; lower bounds how long other
    // actors on the same worker wait behind it. 0 = no limit.
    size_t max_messages_per_turn = 64;

    // How long a pooled worker keeps polling for more work, after running out,
    // before it parks. Work arriving within this window is picked up without
    // an OS wake-up (tens of microseconds on Windows), which keeps
    // request/response latency between pooled actors low: measured round-trip
    // p99.9 ~3 us with a spin, ~27 us without.
    //
    // Cost: roughly (spin length) x (bursts of work per second) of CPU per
    // busy worker - e.g. 20 us x 1000 bursts/s = 2% of a core - and at most
    // one core per worker when bursts arrive just further apart than the
    // spin. Workers that are idle do not spin. 0 = park at once. For the
    // lowest latency on a critical path, use fast_actor.
    std::chrono::microseconds worker_idle_spin{20};

    // DEPRECATED and ignored. Calls are processed on the target actor's
    // own thread so they serialise with regular handlers; there is no separate
    // ask thread pool. Retained so existing configuration code still compiles.
    size_t ask_thread_pool_size = 0;
};

// Configuration for shutdown behavior
struct shutdown_config {
    std::chrono::milliseconds drain_timeout{5000};  // Max time to wait for queues to drain
    std::chrono::milliseconds check_interval{10};   // How often to check if queues are empty
};

// Stop mode for individual actor removal
enum class stop_mode {
    drain,    // Process remaining messages before stopping (default)
    discard   // Drop pending messages immediately
};

// Configuration for stopping individual actors
struct stop_config {
    // drain: keep processing queued messages until the queue is empty or
    //        drain_timeout expires, then stop.
    // discard: drop queued messages (counted as dead letters) and stop.
    stop_mode mode = stop_mode::drain;
    std::chrono::milliseconds drain_timeout{1000};   // Drain budget, from the stop_actor() call
    // true: stop_actor() returns after on_stop() has run.
    // false: stop_actor() returns once the stop is requested.
    // Either way the actor stops accepting messages immediately.
    bool wait_for_stop{true};
    bool notify_watchers{true};                      // Send termination_msg to watchers
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

    // True while worker and fast-actor threads may be executing actors: set in
    // start() before they are launched, cleared in wait_for_shutdown() after
    // they are all joined. When false, no thread but the caller can be
    // executing an actor, so stop_actor() completes a stop itself.
    std::atomic<bool> m_workers_active{false};

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

    // Per-worker scheduling state (design adapted from CAF - see
    // knowledge_base/caf_comparison.md). A worker runs only actors that are on
    // its ready queue; it never scans the actors pinned to it.
    //
    // An actor is put on the queue when a push finds its mailbox blocked
    // (idle) - so once per idle-to-busy transition, not per message - and when
    // a turn ends with messages left (the budget ran out). While it is queued
    // or running, further messages just go into its mailbox.
    //
    // Sized once, together with m_actors_per_thread, and kept until reset():
    // producers index it without a lock, so it must never be resized or freed
    // while any actor can still be scheduled.
    struct worker_state {
        detail::ready_queue ready;  // FIFO across producers - fairness depends on it

        // Parking. A worker with nothing to run sets `sleeping`, re-checks the
        // queue, and only then waits; a producer signals only a worker that has
        // set `sleeping`. Fences on both sides make it impossible for both to
        // miss each other (see park_worker / wake_worker).
        std::atomic<bool> sleeping{false};
        std::mutex mutex;
        std::condition_variable cv;
    };
    std::vector<std::unique_ptr<worker_state>> m_workers;

    // Watch pattern - maps watched_actor -> set of watcher actors
    // Watch pattern (system::watch). Keyed by instance id, not address: a new
    // actor can reuse a destroyed actor's memory. Watchers are held weakly, so
    // one destroyed before the actor it watches is simply skipped.
    //   m_watchers: watched id -> (watcher id -> watcher)
    //   m_watching: watcher id -> ids it watches (reverse index, so a stopping
    //               actor can be removed from everything it was watching)
    // Both are cleared by reset(): instance ids restart there.
    std::unordered_map<size_t, std::unordered_map<size_t, std::weak_ptr<actor>>> m_watchers;
    std::unordered_map<size_t, std::unordered_set<size_t>> m_watching;
    std::mutex m_watchers_mutex;

    // Dead letter tracking (always-on statistics)
    std::atomic<size_t> m_dead_letter_tell{0};
    std::atomic<size_t> m_dead_letter_ask{0};
    std::atomic<size_t> m_dead_letter_invalid{0};

    // Optional dead letter callback handler
    std::shared_ptr<dead_letter_handler> m_dead_letter_handler;
    std::mutex m_dead_letter_handler_mutex;

    // Optional queue threshold callback handler
    std::shared_ptr<queue_threshold_handler> m_queue_threshold_handler;
    std::mutex m_queue_threshold_handler_mutex;

    // Handler exceptions: count, and optional callback
    std::atomic<size_t> m_handler_exceptions{0};
    std::shared_ptr<handler_exception_handler> m_handler_exception_handler;
    std::mutex m_handler_exception_handler_mutex;

    // Singleton instance
    static system& instance();

    // Private constructor for singleton
    system();

    // Worker thread function for thread pool
    void worker_thread(size_t thread_id);

    // Scheduler (see worker_state)
    void ensure_workers(size_t count);           // call with m_actors_mutex held
    void run_actor(worker_state& w, std::shared_ptr<actor> target);
    void park_worker(worker_state& w);
    bool spin_for_work(worker_state& w);         // true if work arrived while spinning
    static void wake_worker(worker_state& w);
    static void activate_after_start(actor* target);  // after on_start(): go idle or schedule

    // INVARIANT 2: copies of the actor lists, taken under their mutex and
    // returned after it is released, for code paths that call into actors.
    static std::vector<std::shared_ptr<actor>> snapshot_pooled_actors();
    static std::vector<std::shared_ptr<actor>> snapshot_fast_actors();

    // Stop protocol helpers (see stop_actor())
    static void run_stop_to_completion(actor* target);
    static void complete_pending_stops();
    static void reap_stopped_fast_actors();
    static bool is_executor_of(actor* target);

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
    //
    // If the system is already running, the actor's on_start() is called before
    // create() returns, so it is ready to receive messages immediately. If the
    // system has not started yet, on_start() is called by start() instead.
    // Either way on_start() runs exactly once.
    //
    // Safe to call from inside a message handler.
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

    // Internal: drop finished and cancelled timers from an actor's set
    static void forget_finished_timers(std::set<timer_id>& ids);

    // Internal: throw if a call() from this thread to target can never be
    // answered - from on_start() (on_start_violation), to itself, or to an
    // actor that only this thread executes (call_deadlock_error). Called by
    // actor_ref::call before blocking.
    static void check_call_allowed(actor* target);

    // Internal: wake the worker thread owning this actor (called by enqueue_message)
    static void notify_worker(size_t thread_id);

    // Internal: put an actor on its worker's ready queue. Called when a push
    // reports that the actor's mailbox was blocked (idle).
    static void schedule(actor* target);

    // Internal: report a queue threshold breach (called by actor::enqueue_message)
    static void report_queue_threshold(const std::string& actor_name,
                                       size_t queue_size,
                                       size_t threshold);

    // Stop a single actor.
    //
    // Returns true if this call initiated the stop; false if the ref is
    // invalid or the actor is already stopping or stopped (including by
    // system shutdown). The actor stops accepting messages before this
    // returns, so is_running() is false immediately.
    //
    // The stop itself - cancelling timers, draining or discarding the queue,
    // on_shutdown(), on_stop(), notifying watchers - runs on the actor's own
    // thread, serialised with its handlers (INVARIANT 1). No handler runs
    // after on_stop().
    //
    // With wait_for_stop, the call blocks until on_stop() has returned. It
    // does not block - the stop completes asynchronously - when waiting
    // would deadlock: when called from the target actor itself, from the
    // worker thread that executes it, from a fast actor's own thread, or
    // after system shutdown has been requested.
    static bool stop_actor(actor_ref ref, const stop_config& config = stop_config{});

    // Stop actor by name (convenience)
    static bool stop_actor(const std::string& name, const stop_config& config = stop_config{});

    // Check if actor is still running
    static bool is_actor_running(actor_ref ref);

    // Watch an actor for termination notification
    // When 'watched' actor stops, 'watcher' receives termination_msg
    static void watch(actor_ref watcher, actor_ref watched);

    // Unwatch an actor
    static void unwatch(actor_ref watcher, actor_ref watched);

    // Internal: when an actor stops - send termination_msg to its watchers (if
    // `notify`) and remove every watch it is part of, as watcher or watched.
    static void end_watches(actor* stopped_actor, bool notify);

    // Internal: equivalent to end_watches(stopped_actor, true).
    static void notify_watchers_internal(actor* stopped_actor);

    // Internal: advance a stop requested by stop_actor(). Called only by the
    // thread executing the actor (see actor::service_stop_request()). Returns
    // false while still draining, true once complete.
    static bool service_stop(actor* target);

    // Dead letter handling
    // Get statistics for dropped messages (always-on, negligible overhead)
    static dead_letter_stats get_dead_letter_stats();

    // Reset dead letter statistics to zero
    static void reset_dead_letter_stats();

    // Set custom handler for dead letters (receives info about each dropped message)
    // Pass nullptr or empty function to clear
    static void set_dead_letter_handler(dead_letter_handler handler);

    // Clear the dead letter handler
    static void clear_dead_letter_handler();

    // Set custom handler for queue threshold breaches
    // Fires once per breach per actor. Pass nullptr or empty function to clear.
    static void set_queue_threshold_handler(queue_threshold_handler handler);

    // Clear the queue threshold handler
    static void clear_queue_threshold_handler();

    /// Set the callback for exceptions thrown by message handlers (pooled,
    /// fast and stateful actors) and by on_shutdown()/on_stop(). Without one,
    /// each is written to stderr. Pass an empty function to clear.
    static void set_handler_exception_handler(handler_exception_handler handler);
    static void clear_handler_exception_handler();

    /// Handler and stop-hook exceptions caught since start (reset by reset())
    static size_t handler_exception_count();

    // Internal: report a caught handler / hook exception
    static void report_handler_exception(const actor& target, std::type_index message_type,
                                         uint64_t message_id, const char* source,
                                         std::exception_ptr error, exception_action action);

    // Internal: report a dead letter (called by actor::enqueue_message)
    static void report_dead_letter(const std::string& actor_name,
                                   const message_base* msg,
                                   actor_state state,
                                   bool was_call);

    // Internal: a tell() or call(timeout) on an invalid actor_ref. Counted in
    // dead_letter_stats::dropped_to_invalid; the dead-letter handler sees an
    // empty target name, message_id 0 and state stopped.
    static void report_invalid_target(std::type_index message_type);
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
