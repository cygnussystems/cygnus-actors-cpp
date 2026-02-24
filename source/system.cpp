#include "cas/system.h"
#include "cas/actor.h"
#include "cas/actor_ref.h"
#include "cas/actor_registry.h"
#include "cas/fast_actor.h"
#include "cas/message_base.h"
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


} // namespace cas
