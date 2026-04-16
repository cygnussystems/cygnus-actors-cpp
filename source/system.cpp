#include "cas/system.h"
#include "cas/actor.h"
#include "cas/actor_ref.h"
#include "cas/actor_registry.h"
#include "cas/fast_actor.h"
#include "cas/message_base.h"
#include "cas/actor_ref_impl.h"
#include <iostream>
#include <stdexcept>

namespace cas {

system::system() {
    // Default configuration - will be set properly in init/configure
}

system::~system() {
    if (m_running) {
        shutdown();
        wait_for_shutdown();
    }
}

system& system::instance() {
    static system sys;
    return sys;
}

void system::init(const system_config& config) {
    auto& inst = instance();
    inst.m_config = config;
}

void system::configure(const system_config& config) {
    auto& inst = instance();
    if (inst.m_running) {
        throw std::runtime_error("Cannot configure system while it's running");
    }
    inst.m_config = config;
}

void system::start() {
    auto& inst = instance();

    if (inst.m_running) {
        return;  // Already running
    }

    inst.m_running = true;
    inst.m_shutdown_requested = false;

    // Determine thread pool size
    size_t thread_count = inst.m_config.thread_pool_size;
    if (thread_count == 0) {
        thread_count = std::thread::hardware_concurrency();
        if (thread_count == 0) thread_count = 4;
    }

    // Initialize per-thread actor lists
    inst.m_actors_per_thread.resize(thread_count);

    // Start timer manager
    inst.m_timer_manager.start();

    // Start all pooled actors (call on_start)
    {
        std::lock_guard<std::mutex> lock(inst.m_actors_mutex);
        for (auto& thread_actors : inst.m_actors_per_thread) {
            for (auto& actor_ptr : thread_actors) {
                // Set actor context so messages sent from on_start have correct sender
                current_actor_context = actor_ptr.get();
                actor_ptr->on_start();
                current_actor_context = nullptr;
            }
        }
    }

    // Create thread pool with thread affinity
    for (size_t i = 0; i < thread_count; ++i) {
        inst.m_thread_pool.emplace_back(&system::worker_thread, &inst, i);
    }

    // Start ask thread pool (dedicated RPC threads)
    size_t ask_thread_count = inst.m_config.ask_thread_pool_size;
    for (size_t i = 0; i < ask_thread_count; ++i) {
        inst.m_ask_thread_pool.emplace_back(&system::ask_worker_thread, &inst);
    }

    // Start all fast actors with dedicated threads
    {
        std::lock_guard<std::mutex> lock(inst.m_fast_actors_mutex);
        for (auto& actor_ptr : inst.m_fast_actors) {
            fast_actor* fast_ptr = static_cast<fast_actor*>(actor_ptr.get());
            // Launch dedicated thread that runs tight polling loop
            fast_ptr->m_dedicated_thread = std::thread(&fast_actor::run_dedicated_thread, fast_ptr);
        }
    }
}

void system::shutdown(const shutdown_config& config) {
    auto& inst = instance();

    // Clear previous shutdown log
    {
        std::lock_guard<std::mutex> lock(inst.m_shutdown_log_mutex);
        inst.m_shutdown_log.clear();
    }

    // Phase 1: Inject shutdown message into all actors
    // Actors will call on_shutdown() and set state=stopping when they process this message
    {
        std::lock_guard<std::mutex> lock(inst.m_actors_mutex);
        for (auto& thread_actors : inst.m_actors_per_thread) {
            for (auto& actor_ptr : thread_actors) {
                auto shutdown_msg = std::make_unique<system_shutdown>();
                actor_ptr->enqueue_message(std::move(shutdown_msg));
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(inst.m_fast_actors_mutex);
        for (auto& actor_ptr : inst.m_fast_actors) {
            auto shutdown_msg = std::make_unique<system_shutdown>();
            actor_ptr->enqueue_message(std::move(shutdown_msg));
        }
    }

    // Phase 2: Monitor queue draining with deadline
    auto start_time = std::chrono::steady_clock::now();
    auto deadline = start_time + config.drain_timeout;

    while (std::chrono::steady_clock::now() < deadline) {
        bool all_empty = true;

        // Check pooled actors
        {
            std::lock_guard<std::mutex> lock(inst.m_actors_mutex);
            for (auto& thread_actors : inst.m_actors_per_thread) {
                for (auto& actor_ptr : thread_actors) {
                    if (actor_ptr->has_messages()) {
                        all_empty = false;
                        break;
                    }
                }
                if (!all_empty) break;
            }
        }

        // Check fast actors
        if (all_empty) {
            std::lock_guard<std::mutex> lock(inst.m_fast_actors_mutex);
            for (auto& actor_ptr : inst.m_fast_actors) {
                if (actor_ptr->has_messages()) {
                    all_empty = false;
                    break;
                }
            }
        }

        if (all_empty) {
            break;  // All queues drained successfully
        }

        std::this_thread::sleep_for(config.check_interval);
    }

    // Phase 3: Check for undrained actors and log warnings
    {
        std::lock_guard<std::mutex> lock(inst.m_actors_mutex);
        for (auto& thread_actors : inst.m_actors_per_thread) {
            for (auto& actor_ptr : thread_actors) {
                size_t remaining = actor_ptr->queue_size();
                if (remaining > 0) {
                    std::string warning = "Actor '" + actor_ptr->name() +
                                        "' has " + std::to_string(remaining) +
                                        " unprocessed messages at shutdown";
                    std::lock_guard<std::mutex> log_lock(inst.m_shutdown_log_mutex);
                    inst.m_shutdown_log.push_back(warning);
                }
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(inst.m_fast_actors_mutex);
        for (auto& actor_ptr : inst.m_fast_actors) {
            size_t remaining = actor_ptr->queue_size();
            if (remaining > 0) {
                std::string warning = "Fast actor '" + actor_ptr->name() +
                                    "' has " + std::to_string(remaining) +
                                    " unprocessed messages at shutdown";
                std::lock_guard<std::mutex> log_lock(inst.m_shutdown_log_mutex);
                inst.m_shutdown_log.push_back(warning);
            }
        }
    }

    // Phase 4: Signal worker threads to stop
    inst.m_shutdown_requested = true;
}

void system::wait_for_shutdown() {
    auto& inst = instance();

    // Stop timer manager
    inst.m_timer_manager.stop();

    // Set m_running to false now that timer manager is stopped
    inst.m_running = false;

    // Wait for all pooled actor threads to finish
    for (auto& thread : inst.m_thread_pool) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    inst.m_thread_pool.clear();

    // Wait for all ask threads to finish
    for (auto& thread : inst.m_ask_thread_pool) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    inst.m_ask_thread_pool.clear();

    // Wait for all fast actor threads to finish
    // Fast actors call their own on_stop() in their dedicated threads
    {
        std::lock_guard<std::mutex> lock(inst.m_fast_actors_mutex);
        for (auto& actor_ptr : inst.m_fast_actors) {
            fast_actor* fast_ptr = static_cast<fast_actor*>(actor_ptr.get());
            if (fast_ptr->m_dedicated_thread.joinable()) {
                fast_ptr->m_dedicated_thread.join();
            }
            // Mark as stopped (on_stop already called in run_dedicated_thread)
            actor_ptr->set_state(actor_state::stopped);
        }
    }

    // Call on_stop on all pooled actors and mark them as stopped
    {
        std::lock_guard<std::mutex> lock(inst.m_actors_mutex);
        for (auto& thread_actors : inst.m_actors_per_thread) {
            for (auto& actor_ptr : thread_actors) {
                // Cancel any active timers for this actor
                cancel_actor_timers(actor_ptr.get());

                // Set actor context (though on_stop shouldn't send messages)
                current_actor_context = actor_ptr.get();
                actor_ptr->on_stop();
                current_actor_context = nullptr;
                actor_ptr->set_state(actor_state::stopped);
            }
        }
    }

    // Note: m_running was already set to false at the start of this function
}

void system::reset() {
    auto& inst = instance();

    // Ensure system is not running
    if (inst.m_running) {
        throw std::runtime_error("Cannot reset system while it's running - call shutdown() and wait_for_shutdown() first");
    }

    // Clear all pooled actors
    {
        std::lock_guard<std::mutex> lock(inst.m_actors_mutex);
        inst.m_actors_per_thread.clear();
    }

    // Clear all fast actors
    {
        std::lock_guard<std::mutex> lock(inst.m_fast_actors_mutex);
        inst.m_fast_actors.clear();
    }

    // Clear actor registry
    actor_registry::clear();

    // Reset instance ID counter
    inst.m_next_instance_id.store(1);

    // Reset thread assignment counter
    inst.m_next_thread_assignment.store(0);

    // Reset flags
    inst.m_shutdown_requested = false;

    // Note: m_config is preserved across resets
}

bool system::is_running() {
    return instance().m_running;
}

size_t system::actor_count() {
    auto& inst = instance();

    size_t total = 0;

    // Count pooled actors
    {
        std::lock_guard<std::mutex> lock(inst.m_actors_mutex);
        for (const auto& thread_actors : inst.m_actors_per_thread) {
            total += thread_actors.size();
        }
    }

    // Count fast actors
    {
        std::lock_guard<std::mutex> lock(inst.m_fast_actors_mutex);
        total += inst.m_fast_actors.size();
    }

    return total;
}

std::vector<std::string> system::get_shutdown_log() {
    auto& inst = instance();
    std::lock_guard<std::mutex> lock(inst.m_shutdown_log_mutex);
    return inst.m_shutdown_log;
}

void system::register_actor(std::shared_ptr<actor> actor_ptr) {
    auto& inst = instance();

    // Assign unique instance ID (do this FIRST, before any virtual calls)
    actor_ptr->m_instance_id = inst.m_next_instance_id.fetch_add(1);

    // Set the actor's self reference
    actor_ptr->set_self_ref(actor_ptr);

    // Set queue threshold from system config
    actor_ptr->set_queue_threshold(inst.m_config.queue_threshold);

    // Check if this is a fast actor
    fast_actor* fast_ptr = dynamic_cast<fast_actor*>(actor_ptr.get());
    if (fast_ptr) {
        // Add to fast actors list
        std::lock_guard<std::mutex> lock(inst.m_fast_actors_mutex);
        inst.m_fast_actors.push_back(actor_ptr);
    } else {
        // Regular pooled actor - assign to thread pool
        std::lock_guard<std::mutex> lock(inst.m_actors_mutex);

        // Assign actor to a thread (round-robin)
        size_t num_threads = inst.m_actors_per_thread.empty() ? 1 : inst.m_actors_per_thread.size();
        size_t thread_id = inst.m_next_thread_assignment.fetch_add(1) % num_threads;

        // Ensure we have enough thread vectors
        if (inst.m_actors_per_thread.size() < num_threads) {
            inst.m_actors_per_thread.resize(num_threads);
        }

        // Set thread affinity and add to that thread's list
        actor_ptr->set_thread_affinity(thread_id);
        inst.m_actors_per_thread[thread_id].push_back(actor_ptr);
    }
}

system* system::get_instance() {
    return &instance();
}

uint64_t system::next_message_id() {
    return instance().m_next_message_id.fetch_add(1);
}

void system::worker_thread(size_t thread_id) {
    // Each worker thread only processes actors assigned to it
#ifdef CAS_DEBUG_LOGGING
    std::cout << "[WORKER-" << thread_id << "] Thread started\n" << std::flush;
#endif

    while (!m_shutdown_requested) {
        bool found_work = false;

        // Process actors assigned to this thread
        // Hold lock while iterating - actor registration is rare so contention is minimal
        {
            std::lock_guard<std::mutex> lock(m_actors_mutex);
            if (thread_id < m_actors_per_thread.size()) {
                for (auto& actor_ptr : m_actors_per_thread[thread_id]) {
                    if (actor_ptr->has_messages()) {
#ifdef CAS_DEBUG_LOGGING
                        std::cout << "[WORKER-" << thread_id << "] Processing message\n" << std::flush;
#endif
                        actor_ptr->process_next_message();
                        found_work = true;
                    }
                }
            }
        }

        // If no work found, sleep briefly to avoid busy-waiting
        // TODO: Use condition variable instead
        if (!found_work) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

#ifdef CAS_DEBUG_LOGGING
    std::cout << "[WORKER-" << thread_id << "] Thread stopping\n" << std::flush;
#endif
}

void system::ask_worker_thread() {
    // Ask worker threads process ask requests from the global queue
#ifdef CAS_DEBUG_LOGGING
    std::cout << "[ASK-WORKER] Thread started\n" << std::flush;
#endif

    while (!m_shutdown_requested) {
        ask_request_envelope envelope;

        // Try to dequeue an ask request
        if (m_global_ask_queue.try_dequeue(envelope)) {
#ifdef CAS_DEBUG_LOGGING
            std::cout << "[ASK-WORKER] Processing ask request\n" << std::flush;
#endif
            // Process the ask request on the target actor
            // Note: We're calling dispatch directly, NOT through the regular message queue
            envelope.target->dispatch_message(envelope.request.get());
        } else {
            // No work - sleep briefly
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

#ifdef CAS_DEBUG_LOGGING
    std::cout << "[ASK-WORKER] Thread stopping\n" << std::flush;
#endif
}

timer_id system::schedule_timer(actor* target, std::unique_ptr<message_base> msg,
                                std::function<std::unique_ptr<message_base>()> copy_func,
                                std::chrono::milliseconds delay,
                                std::chrono::milliseconds interval) {
    auto& inst = instance();

    // Create callback that delivers message to target actor
    auto callback = [target](timer_id, std::unique_ptr<message_base> fired_msg) {
        target->enqueue_message(std::move(fired_msg));
    };

    // Delegate to timer_manager
    return inst.m_timer_manager.schedule(
        std::move(msg),
        std::move(copy_func),
        delay,
        interval,
        std::move(callback)
    );
}

void system::cancel_timer_internal(timer_id id) {
    auto& inst = instance();
    inst.m_timer_manager.cancel(id);
}

void system::cancel_actor_timers(actor* target) {
    auto& inst = instance();

    // Cancel all timers that belong to this actor
    // The actor tracks its own timer IDs in m_active_timers
    for (timer_id id : target->m_active_timers) {
        inst.m_timer_manager.cancel(id);
    }
    target->m_active_timers.clear();
}

void system::enqueue_global_ask(actor* target, std::unique_ptr<message_base> request) {
    auto& inst = instance();

    // Enqueue to global ask queue
    ask_request_envelope envelope{target, std::move(request)};
    inst.m_global_ask_queue.enqueue(std::move(envelope));
}

bool system::stop_actor(actor_ref ref, const stop_config& config) {
    auto& inst = instance();

    // Step 1: Validate actor_ref and get raw pointer
    actor* actor_ptr = ref.get<actor>();
    if (!actor_ptr) {
        return false;  // Invalid reference
    }

    // Step 2: Check current state - if already stopping/stopped, return false
    actor_state expected = actor_state::running;
    if (!actor_ptr->m_state.compare_exchange_strong(expected, actor_state::stopping)) {
        return false;  // Already stopping or stopped (another thread got here first)
    }

    // Step 3: Cancel all timers for this actor
    cancel_actor_timers(actor_ptr);

    // Step 4: Handle message draining based on config.mode
    if (config.mode == stop_mode::drain && config.wait_for_stop) {
        // Wait for queues to empty with timeout
        auto start_time = std::chrono::steady_clock::now();
        auto deadline = start_time + config.drain_timeout;

        while (std::chrono::steady_clock::now() < deadline) {
            if (!actor_ptr->has_messages()) {
                break;  // Queue drained successfully
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    // If discard mode or async, skip draining

    // Step 5: Call on_shutdown() hook (actor can send final messages)
    // Set thread-local context so actor knows it's the current actor
    actor* prev_context = current_actor_context;
    current_actor_context = actor_ptr;
    actor_ptr->on_shutdown();
    current_actor_context = prev_context;

    // Step 6: Call on_stop() hook (final cleanup, no messaging)
    current_actor_context = actor_ptr;
    actor_ptr->on_stop();
    current_actor_context = prev_context;

    // Step 7: Notify watchers if configured
    if (config.notify_watchers) {
        notify_watchers_internal(actor_ptr);
    }

    // Step 8: Remove from thread's actor list
    // Determine if this is a fast_actor or pooled actor
    fast_actor* fast_ptr = dynamic_cast<fast_actor*>(actor_ptr);

    if (fast_ptr) {
        // Fast actor - remove from m_fast_actors and join thread
        std::lock_guard<std::mutex> lock(inst.m_fast_actors_mutex);

        // Join dedicated thread if needed
        if (fast_ptr->m_dedicated_thread.joinable()) {
            // Thread should exit naturally when it sees state == stopping
            fast_ptr->m_dedicated_thread.join();
        }

        // Remove from vector
        auto it = std::find_if(inst.m_fast_actors.begin(), inst.m_fast_actors.end(),
                               [actor_ptr](const std::shared_ptr<actor>& ptr) {
                                   return ptr.get() == actor_ptr;
                               });
        if (it != inst.m_fast_actors.end()) {
            inst.m_fast_actors.erase(it);
        }
    } else {
        // Pooled actor - remove from m_actors_per_thread
        std::lock_guard<std::mutex> lock(inst.m_actors_mutex);

        size_t thread_id = actor_ptr->m_assigned_thread_id;
        if (thread_id < inst.m_actors_per_thread.size()) {
            auto& thread_actors = inst.m_actors_per_thread[thread_id];

            // Remove from vector
            auto it = std::find_if(thread_actors.begin(), thread_actors.end(),
                                   [actor_ptr](const std::shared_ptr<actor>& ptr) {
                                       return ptr.get() == actor_ptr;
                                   });
            if (it != thread_actors.end()) {
                thread_actors.erase(it);
            }
        }
    }

    // Step 9: Unregister from actor_registry (if registered)
    std::string actor_name = actor_ptr->name();
    if (!actor_name.empty()) {
        actor_registry::unregister_actor(actor_name);
    }

    // Step 10: Set state to stopped
    actor_ptr->set_state(actor_state::stopped);

    // Step 11: Release shared_ptr happens automatically when we return
    // (the shared_ptr in the vector was removed, but caller may still hold a reference)

    return true;  // Success
}

bool system::stop_actor(const std::string& name, const stop_config& config) {
    auto ref = actor_registry::get(name);
    if (!ref.is_valid()) {
        return false;
    }
    return stop_actor(ref, config);
}

bool system::is_actor_running(actor_ref ref) {
    actor* ptr = ref.get<actor>();
    if (!ptr) {
        return false;
    }
    return ptr->get_state() == actor_state::running;
}

void system::watch(actor_ref watcher, actor_ref watched) {
    auto& inst = instance();

    // Get raw pointers
    actor* watcher_ptr = watcher.get<actor>();
    actor* watched_ptr = watched.get<actor>();

    // Validate both actors exist
    if (!watcher_ptr || !watched_ptr) {
        return;  // Invalid reference
    }

    // Add watcher to watchers set for watched actor
    std::lock_guard<std::mutex> lock(inst.m_watchers_mutex);
    inst.m_watchers[watched_ptr].insert(watcher_ptr);
}

void system::unwatch(actor_ref watcher, actor_ref watched) {
    auto& inst = instance();

    // Get raw pointers
    actor* watcher_ptr = watcher.get<actor>();
    actor* watched_ptr = watched.get<actor>();

    // Validate both actors exist
    if (!watcher_ptr || !watched_ptr) {
        return;  // Invalid reference
    }

    // Remove watcher from watchers set
    std::lock_guard<std::mutex> lock(inst.m_watchers_mutex);

    auto it = inst.m_watchers.find(watched_ptr);
    if (it != inst.m_watchers.end()) {
        it->second.erase(watcher_ptr);

        // Clean up empty entry
        if (it->second.empty()) {
            inst.m_watchers.erase(it);
        }
    }
}

void system::notify_watchers_internal(actor* stopped_actor) {
    auto& inst = instance();

    // Copy the watcher set while holding lock (to release lock quickly)
    std::unordered_set<actor*> watchers_copy;
    {
        std::lock_guard<std::mutex> lock(inst.m_watchers_mutex);

        auto it = inst.m_watchers.find(stopped_actor);
        if (it != inst.m_watchers.end()) {
            watchers_copy = it->second;
            // Remove entry from map (watched actor is stopping)
            inst.m_watchers.erase(it);
        }
    }

    // Send termination_msg to each watcher (without holding lock)
    for (actor* watcher_ptr : watchers_copy) {
        termination_msg msg;
        msg.actor_name = stopped_actor->name();
        msg.instance_id = stopped_actor->instance_id();
        msg.type_name = stopped_actor->type_name();

        // Create actor_ref and send message
        actor_ref watcher_ref(watcher_ptr->m_self_ref.lock());
        if (watcher_ref.is_valid()) {
            watcher_ref.tell(msg);
        }
    }
}

// Dead letter handling implementation

dead_letter_stats system::get_dead_letter_stats() {
    auto& inst = instance();

    dead_letter_stats stats;
    stats.dropped_tell = inst.m_dead_letter_tell.load(std::memory_order_relaxed);
    stats.dropped_ask = inst.m_dead_letter_ask.load(std::memory_order_relaxed);
    stats.dropped_to_invalid = inst.m_dead_letter_invalid.load(std::memory_order_relaxed);
    return stats;
}

void system::reset_dead_letter_stats() {
    auto& inst = instance();

    inst.m_dead_letter_tell.store(0, std::memory_order_relaxed);
    inst.m_dead_letter_ask.store(0, std::memory_order_relaxed);
    inst.m_dead_letter_invalid.store(0, std::memory_order_relaxed);
}

void system::set_dead_letter_handler(dead_letter_handler handler) {
    auto& inst = instance();

    std::lock_guard<std::mutex> lock(inst.m_dead_letter_handler_mutex);
    if (handler) {
        inst.m_dead_letter_handler = std::make_shared<dead_letter_handler>(std::move(handler));
    } else {
        inst.m_dead_letter_handler.reset();
    }
}

void system::clear_dead_letter_handler() {
    auto& inst = instance();

    std::lock_guard<std::mutex> lock(inst.m_dead_letter_handler_mutex);
    inst.m_dead_letter_handler.reset();
}

void system::report_dead_letter(const std::string& actor_name,
                                 const message_base* msg,
                                 actor_state state,
                                 bool was_ask) {
    auto& inst = instance();

    // Always increment counter (negligible overhead)
    if (was_ask) {
        inst.m_dead_letter_ask.fetch_add(1, std::memory_order_relaxed);
    } else {
        inst.m_dead_letter_tell.fetch_add(1, std::memory_order_relaxed);
    }

    // Optional logging (controlled by config)
    if (inst.m_config.log_dead_letters) {
        const char* state_str = (state == actor_state::stopping) ? "stopping" : "stopped";
        std::cerr << "[DEAD LETTER] Message " << msg->message_id
                  << " to actor '" << actor_name << "' dropped (state: "
                  << state_str << ")\n";
    }

    // Optional callback handler
    std::shared_ptr<dead_letter_handler> handler;
    {
        std::lock_guard<std::mutex> lock(inst.m_dead_letter_handler_mutex);
        handler = inst.m_dead_letter_handler;
    }

    if (handler && *handler) {
        dead_letter_info info(actor_name, typeid(*msg), msg->message_id, state);
        (*handler)(info);
    }
}

} // namespace cas
