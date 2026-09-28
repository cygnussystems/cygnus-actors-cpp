#include "cas/system.h"
#include "cas/actor.h"
#include "cas/actor_ref.h"
#include "cas/actor_registry.h"
#include "cas/fast_actor.h"
#include "cas/message_base.h"
#include "cas/actor_ref_impl.h"
#include "cpu_pause.h"
#include <iostream>
#include <stdexcept>
#include <algorithm>
#include <limits>

namespace cas {

namespace {

// Index of the pool worker running on this thread, or npos on any other
// thread. Lets stop_actor() recognise that it is running on the very thread
// that must execute the stop, where blocking would deadlock.
constexpr size_t not_a_worker = std::numeric_limits<size_t>::max();
thread_local size_t t_worker_thread_id = not_a_worker;

} // namespace

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

    // Initialize per-thread actor lists. register_actor() may have already
    // sized this using the same width; only grow, never shrink, so actors
    // registered before start() are never discarded.
    {
        std::lock_guard<std::mutex> lock(inst.m_actors_mutex);
        if (inst.m_actors_per_thread.size() < thread_count) {
            inst.m_actors_per_thread.resize(thread_count);
        } else {
            thread_count = inst.m_actors_per_thread.size();
        }
        inst.ensure_workers(thread_count);
        // Entries left from a previous run (workers exit on shutdown without
        // draining their queues) are stale: this run starts from scratch.
        for (auto& worker : inst.m_workers) {
            worker->ready.clear();
        }
    }

    // Start timer manager
    inst.m_timer_manager.start();

    // Start all pooled actors (call on_start).
    // INVARIANT 2: snapshot under the lock, release, then call on_start().
    // on_start() is user code and may call system::create(), which re-enters
    // register_actor() and takes m_actors_mutex - holding it here would
    // self-deadlock. register_actor() defers on_start() for the same reason.
    std::vector<std::shared_ptr<actor>> to_start;
    {
        std::lock_guard<std::mutex> lock(inst.m_actors_mutex);
        for (auto& thread_actors : inst.m_actors_per_thread) {
            to_start.insert(to_start.end(), thread_actors.begin(), thread_actors.end());
        }
    }

    for (auto& actor_ptr : to_start) {
        {
            // Sets sender context and marks the actor as initialising
            on_start_scope scope(actor_ptr.get());
            actor_ptr->on_start();
        }
        // Idle, or on the ready queue if messages were sent to it before
        // start(); either way the workers launched below pick it up.
        activate_after_start(actor_ptr.get());
    }

    // From here on, actors may be executing on threads other than the caller's
    inst.m_workers_active.store(true, std::memory_order_release);

    // Create thread pool with thread affinity
    for (size_t i = 0; i < thread_count; ++i) {
        inst.m_thread_pool.emplace_back(&system::worker_thread, &inst, i);
    }

    // Note: no ask thread pool. Calls are drained by the target actor's
    // own worker thread so they serialise with regular handlers (INVARIANT 1).

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
                // An actor mid-stop_actor() is already stopping; sending it
                // system_shutdown would only register a spurious dead letter.
                if (actor_ptr->get_state() != actor_state::running) continue;
                auto shutdown_msg = std::make_unique<system_shutdown>();
                actor_ptr->enqueue_message(std::move(shutdown_msg));
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(inst.m_fast_actors_mutex);
        for (auto& actor_ptr : inst.m_fast_actors) {
            if (actor_ptr->get_state() != actor_state::running) continue;
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

    // Phase 4: Signal worker threads to stop, then wake any that are parked
    inst.m_shutdown_requested = true;

    for (auto& worker : inst.m_workers) {
        {
            std::lock_guard<std::mutex> lock(worker->mutex);
        }
        worker->cv.notify_all();
    }
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
    // m_workers is kept: producers may still index it (see worker_state).

    // Wait for all fast actor threads to finish.
    // Fast actors call their own on_stop() in their dedicated threads.
    // Joined OUTSIDE m_fast_actors_mutex: a fast actor's on_stop() may call
    // back into the system (e.g. actor_count()), which takes that mutex.
    std::vector<std::shared_ptr<actor>> fast_snapshot;
    {
        std::lock_guard<std::mutex> lock(inst.m_fast_actors_mutex);
        fast_snapshot = inst.m_fast_actors;
    }
    for (auto& actor_ptr : fast_snapshot) {
        fast_actor* fast_ptr = static_cast<fast_actor*>(actor_ptr.get());
        if (!fast_ptr->m_join_claimed.exchange(true) &&
            fast_ptr->m_dedicated_thread.joinable()) {
            fast_ptr->m_dedicated_thread.join();
        }
    }

    // Every executor thread is gone. Stops that were requested but not
    // carried out (workers exit once shutdown is requested) are completed
    // here, on this thread, which is now the only one touching actors.
    inst.m_workers_active.store(false, std::memory_order_release);
    complete_pending_stops();

    for (auto& actor_ptr : fast_snapshot) {
        // on_stop() already ran on the dedicated thread (or in the stop above)
        if (actor_ptr->get_state() != actor_state::stopped) {
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

    // Clear all pooled actors and the per-worker state (queued entries hold
    // shared_ptrs to actors)
    {
        std::lock_guard<std::mutex> lock(inst.m_actors_mutex);
        inst.m_actors_per_thread.clear();
        inst.m_workers.clear();
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

    // Fast actors stopped asynchronously stay listed until their thread is
    // joined; do that now so they are not counted.
    reap_stopped_fast_actors();

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

    // Actors registered while the system is already running must be started
    // here - system::start() has already run and will not revisit them.
    // Deliberately deferred until after the registration locks are released:
    // on_start() is user code and may call system::create() (INVARIANT 2).
    const bool needs_start = inst.m_running.load();

    // Check if this is a fast actor
    fast_actor* fast_ptr = dynamic_cast<fast_actor*>(actor_ptr.get());
    if (fast_ptr) {
        // Add to fast actors list
        std::lock_guard<std::mutex> lock(inst.m_fast_actors_mutex);
        inst.m_fast_actors.push_back(actor_ptr);
    } else {
        // Regular pooled actor - assign to thread pool
        std::lock_guard<std::mutex> lock(inst.m_actors_mutex);

        // Resolve the pool width the same way start() will. Actors are usually
        // created BEFORE start(), when m_actors_per_thread is still empty -
        // treating that as a width of 1 would pin every pre-start actor to
        // thread 0 and leave the rest of the pool idle.
        size_t num_threads = inst.m_actors_per_thread.size();
        if (num_threads == 0) {
            num_threads = inst.m_config.thread_pool_size;
            if (num_threads == 0) {
                num_threads = std::thread::hardware_concurrency();
                if (num_threads == 0) num_threads = 4;
            }
        }

        // Assign actor to a thread (round-robin)
        size_t thread_id = inst.m_next_thread_assignment.fetch_add(1) % num_threads;

        // Ensure we have enough thread vectors (and matching worker state)
        if (inst.m_actors_per_thread.size() < num_threads) {
            inst.m_actors_per_thread.resize(num_threads);
        }
        inst.ensure_workers(inst.m_actors_per_thread.size());

        // Set thread affinity and add to that thread's list
        actor_ptr->set_thread_affinity(thread_id);
        inst.m_actors_per_thread[thread_id].push_back(actor_ptr);
    }

    // Start the actor if the system is already running. No lock is held here.
    if (needs_start) {
        {
            on_start_scope scope(actor_ptr.get());
            actor_ptr->on_start();
        }
        activate_after_start(actor_ptr.get());

        if (fast_ptr) {
            // A fast actor needs its dedicated polling thread launched; start()
            // already ran and will not do it for this one.
            std::lock_guard<std::mutex> lock(inst.m_fast_actors_mutex);
            if (!fast_ptr->m_dedicated_thread.joinable()) {
                fast_ptr->m_dedicated_thread =
                    std::thread(&fast_actor::run_dedicated_thread_started, fast_ptr);
            }
        }
    }
}

system* system::get_instance() {
    return &instance();
}

uint64_t system::next_message_id() {
    return instance().m_next_message_id.fetch_add(1);
}

void system::ensure_workers(size_t count) {
    // Called with m_actors_mutex held, while no actor can be scheduled yet
    // (see worker_state): only ever grows from empty.
    if (m_workers.empty()) {
        m_workers.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            m_workers.push_back(std::make_unique<worker_state>());
        }
    }
}

void system::schedule(actor* target) {
    // Fast actors run their own loop and are never scheduled here.
    if (dynamic_cast<fast_actor*>(target) != nullptr) {
        return;
    }
    std::shared_ptr<actor> keep_alive = target->m_self_ref.lock();
    if (!keep_alive) {
        return;  // being destroyed
    }
    auto& inst = instance();
    if (target->m_assigned_thread_id >= inst.m_workers.size()) {
        return;  // not yet sized: start() schedules pending actors itself
    }
    worker_state& w = *inst.m_workers[target->m_assigned_thread_id];
    w.ready.push(std::move(keep_alive));
    wake_worker(w);
}

void system::activate_after_start(actor* target) {
    // During on_start() the mailbox is deliberately left unblocked, so pushes
    // queue without scheduling and no worker can run the actor while on_start()
    // is still executing on the creator's thread (INVARIANT 1). Now either go
    // idle, or - if messages arrived meanwhile - get scheduled.
    if (dynamic_cast<fast_actor*>(target) != nullptr) {
        return;  // polls its own mailbox
    }
    if (!target->m_mailbox.try_block()) {
        schedule(target);
    }
}

void system::wake_worker(worker_state& w) {
    // Producer half of the parking handshake (consumer half: park_worker).
    // The item is already on w.ready; the fence orders that enqueue before the
    // load of `sleeping`. Either this load sees the worker parking, or the
    // worker's re-check after setting `sleeping` sees the item.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (w.sleeping.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lock(w.mutex);
        w.cv.notify_one();
    }
}

bool system::spin_for_work(worker_state& w) {
    // Poll without sleeping for up to worker_idle_spin. A worker spinning here
    // has not set `sleeping`, so producers do not signal it - it just sees the
    // queue fill. The clock is read only every 64 polls.
    const auto spin = m_config.worker_idle_spin;
    if (spin.count() <= 0) {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + spin;
    for (unsigned i = 1;; ++i) {
        if (w.ready.maybe_nonempty() || m_shutdown_requested.load(std::memory_order_relaxed)) {
            return true;
        }
        CAS_CPU_PAUSE();
        if ((i & 63) == 0 && std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
    }
}

void system::park_worker(worker_state& w) {
    w.sleeping.store(true, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    {
        std::unique_lock<std::mutex> lock(w.mutex);
        // The predicate is evaluated under the mutex, and wake_worker notifies
        // under it, so a wake-up cannot fall between the check and the wait.
        // The timeout is only a backstop against a bug in that handshake.
        w.cv.wait_for(lock, std::chrono::milliseconds(100), [&] {
            return w.ready.maybe_nonempty() ||
                   m_shutdown_requested.load(std::memory_order_relaxed);
        });
    }
    w.sleeping.store(false, std::memory_order_relaxed);
}

void system::run_actor(worker_state& w, std::shared_ptr<actor> target) {
    actor* a = target.get();
    if (a->get_state() == actor_state::stopped) {
        return;  // stale entry for an actor that has since stopped
    }

    const size_t budget = m_config.max_messages_per_turn == 0
                              ? std::numeric_limits<size_t>::max()
                              : m_config.max_messages_per_turn;
    size_t handled = 0;

    for (;;) {
        // A stop requested by stop_actor() is carried out here, on the owning
        // thread, interleaved with draining (INVARIANT 1).
        if (a->m_stop_phase.load(std::memory_order_seq_cst) == actor::stop_phase::requested) {
            if (a->service_stop_request()) {
                return;  // stopped (or completed elsewhere); never runs again
            }
        }

        if (handled >= budget) {
            // Turn over: go to the back of the queue so other ready actors on
            // this worker run. The mailbox stays unblocked, so nothing else will
            // schedule it - we must.
            w.ready.push(std::move(target));
            return;
        }

        if (a->has_messages()) {
            a->process_next_message();
            ++handled;
            continue;
        }

        // Nothing left: go idle. The next push will find the mailbox blocked
        // and schedule the actor again.
        if (a->m_mailbox.try_block()) {
            // A stop_actor() that ran before we blocked could not unblock us
            // (we were running), so it relies on this re-check.
            if (a->m_stop_phase.load(std::memory_order_seq_cst) == actor::stop_phase::requested &&
                a->m_mailbox.try_unblock()) {
                continue;
            }
            return;
        }
        // try_block failed: a push landed meanwhile - keep going.
    }
}

void system::worker_thread(size_t thread_id) {
    // Each worker runs only actors pinned to it, and only when they are ready.
    // No lock is taken on this path (INVARIANT 2 holds trivially).
    t_worker_thread_id = thread_id;
    worker_state& w = *m_workers[thread_id];

    // Spin only after doing work: that is when more is likely to follow soon.
    // A worker that wakes from its backstop timeout with nothing to do goes
    // straight back to sleep - otherwise every idle worker would burn a full
    // spin on each backstop wake-up.
    bool worked = false;
    while (!m_shutdown_requested.load(std::memory_order_relaxed)) {
        if (std::shared_ptr<actor> target = w.ready.pop()) {
            run_actor(w, std::move(target));
            worked = true;
            continue;
        }
        if (worked && spin_for_work(w)) {
            continue;
        }
        worked = false;
        park_worker(w);
    }
}

void system::notify_worker(size_t thread_id) {
    auto& inst = instance();
    if (thread_id < inst.m_workers.size()) {
        wake_worker(*inst.m_workers[thread_id]);
    }
}

timer_id system::schedule_timer(actor* target, std::unique_ptr<message_base> msg,
                                std::function<std::unique_ptr<message_base>()> copy_func,
                                std::chrono::milliseconds delay,
                                std::chrono::milliseconds interval) {
    auto& inst = instance();

    // Create callback that delivers the message to the target actor.
    //
    // Captures a weak_ptr, NOT the raw target pointer. Cancellation cannot
    // stop a callback that is already in flight: the timer thread copies the
    // callback out under the timer mutex, releases the mutex, and only then
    // invokes it (see timer_manager::timer_thread_func). If the actor is
    // stopped and destroyed inside that window - which stop_actor() can do,
    // since it does not wait for an extracted callback to finish - a raw
    // pointer here would be dangling by the time it is dereferenced.
    //
    // Locking the weak_ptr turns that race into a no-op: either the actor is
    // still alive and the shared_ptr keeps it alive across the delivery, or
    // it is gone and the timer message is dropped, which is the correct
    // outcome for a timer belonging to a dead actor.
    std::weak_ptr<actor> weak_target = target->m_self_ref;

    auto callback = [weak_target](timer_id, std::unique_ptr<message_base> fired_msg) {
        if (auto alive = weak_target.lock()) {
            alive->enqueue_message(std::move(fired_msg));
        }
        // else: the actor was destroyed between the callback being extracted
        // and invoked. Dropping the message is correct - there is nobody left
        // to deliver it to.
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

void system::report_queue_threshold(const std::string& actor_name,
                                     size_t queue_size,
                                     size_t threshold) {
    auto& inst = instance();

    std::shared_ptr<queue_threshold_handler> handler;
    {
        std::lock_guard<std::mutex> lock(inst.m_queue_threshold_handler_mutex);
        handler = inst.m_queue_threshold_handler;
    }

    if (handler && *handler) {
        queue_threshold_info info(actor_name, queue_size, threshold);
        (*handler)(info);
    }
}

void system::set_queue_threshold_handler(queue_threshold_handler handler) {
    auto& inst = instance();

    std::lock_guard<std::mutex> lock(inst.m_queue_threshold_handler_mutex);
    if (handler) {
        inst.m_queue_threshold_handler = std::make_shared<queue_threshold_handler>(std::move(handler));
    } else {
        inst.m_queue_threshold_handler.reset();
    }
}

void system::clear_queue_threshold_handler() {
    auto& inst = instance();

    std::lock_guard<std::mutex> lock(inst.m_queue_threshold_handler_mutex);
    inst.m_queue_threshold_handler.reset();
}

bool system::stop_actor(actor_ref ref, const stop_config& config) {
    auto& inst = instance();

    // Hold a reference for the duration: the stop removes the system's own
    // shared_ptr, and an async caller may drop theirs as soon as we return.
    std::shared_ptr<actor> keep_alive = ref.get_actor();
    actor* target = keep_alive.get();
    if (!target) {
        return false;  // Invalid reference
    }

    // Claim the stop. Only one stop_actor() may claim an actor, and never one
    // that system shutdown has claimed (see actor::dispatch_message()).
    if (target->get_state() != actor_state::running) {
        return false;
    }
    auto expected_phase = actor::stop_phase::none;
    if (!target->m_stop_phase.compare_exchange_strong(expected_phase,
                                                      actor::stop_phase::claiming)) {
        return false;  // Another stop_actor() or system shutdown got here first
    }

    // Record the request. Published to the executing thread by the release
    // store of stop_phase::requested below.
    target->m_stop_discard = (config.mode == stop_mode::discard);
    target->m_stop_notify_watchers = config.notify_watchers;
    target->m_stop_drain_deadline = std::chrono::steady_clock::now() + config.drain_timeout;

    // Stop accepting messages now: from here, tell() and call() to this actor
    // are dead letters, so the drain below terminates.
    actor_state expected_state = actor_state::running;
    if (!target->m_state.compare_exchange_strong(expected_state, actor_state::stopping)) {
        // Stopped by wait_for_shutdown() in the meantime
        target->m_stop_phase.store(actor::stop_phase::none, std::memory_order_release);
        return false;
    }

    target->m_stop_phase.store(actor::stop_phase::requested, std::memory_order_seq_cst);

    fast_actor* fast_ptr = dynamic_cast<fast_actor*>(target);

    if (!inst.m_workers_active.load(std::memory_order_acquire)) {
        // No worker or fast-actor thread is running (before start(), or after
        // wait_for_shutdown()), so nothing else can be executing this actor.
        // The caller's thread is its executor; complete the stop here.
        run_stop_to_completion(target);
    } else {
        // An idle actor is on no ready queue: unblock it and schedule it so its
        // worker performs the stop. If it is running or queued, try_unblock
        // fails and the worker sees the requested phase (run_actor re-checks it
        // after blocking). Fast actors poll the phase themselves.
        if (!fast_ptr && target->m_mailbox.try_unblock()) {
            schedule(target);
        }

        // Block until on_stop() has returned - unless this thread is the one
        // that has to perform the stop, or the executors may already have
        // exited for shutdown. In those cases the stop completes
        // asynchronously: on the owning thread once the current handler
        // returns, or in wait_for_shutdown().
        const bool can_wait = config.wait_for_stop &&
                              !inst.m_shutdown_requested.load() &&
                              !is_executor_of(target);
        if (can_wait) {
            actor_state observed = target->m_state.load(std::memory_order_acquire);
            while (observed != actor_state::stopped) {
                target->m_state.wait(observed, std::memory_order_acquire);
                observed = target->m_state.load(std::memory_order_acquire);
            }
        }
    }

    if (fast_ptr && target->get_state() == actor_state::stopped) {
        reap_stopped_fast_actors();
    }

    return true;
}

bool system::service_stop(actor* target) {
    auto& inst = instance();

    // Claim the executor role on first entry. The owning thread calls this
    // repeatedly while draining; a fallback path (stop_actor() with no
    // executor running, or wait_for_shutdown()) may race another fallback,
    // and exactly one of them may complete the stop.
    const std::thread::id self = std::this_thread::get_id();
    std::thread::id executor = target->m_stop_executor.load();
    if (executor != self) {
        if (executor != std::thread::id{} ||
            !target->m_stop_executor.compare_exchange_strong(executor, self)) {
            return true;  // Another thread is completing (or completed) this stop
        }

        // First entry: nothing is delivered to this actor from here on.
        cancel_actor_timers(target);
        if (target->m_stop_discard) {
            target->discard_pending_messages();
        }
    }

    if (target->get_state() == actor_state::stopped) {
        return true;
    }

    // Drain: keep going while there is work and budget left. The caller
    // processes one message and calls again.
    if (!target->m_stop_discard && target->has_messages() &&
        std::chrono::steady_clock::now() < target->m_stop_drain_deadline) {
        return false;
    }

    // Final hooks, on this - the executing - thread.
    actor* prev_context = current_actor_context;
    current_actor_context = target;
    target->on_shutdown();
    target->on_stop();
    current_actor_context = prev_context;

    // on_shutdown() may have scheduled timers; the actor is gone, cancel them.
    cancel_actor_timers(target);

    // Close the mailbox. From here a push fails atomically instead of landing
    // in a mailbox nobody will read: before, a tell() that passed the running
    // check just as the stop completed was silently lost. Anything still queued
    // - left over after a drain timeout, or pushed during the stop - is a dead
    // letter. (A pending system_shutdown is control traffic, not a message.)
    target->m_mailbox.close([target](message_base& msg, bool is_call) {
        if (msg.cas_framework_kind() != message_base::framework_kind::shutdown) {
            report_dead_letter(target->name(), &msg, actor_state::stopping, is_call);
        }
    });

    // Unregister BEFORE notifying watchers: a supervisor typically reacts to
    // termination_msg by creating a replacement under the same name, and
    // that registration must not be removed by this one. The conditional
    // form also leaves alone a name another actor took in the meantime.
    const std::string& actor_name = target->name();
    if (!actor_name.empty()) {
        actor_registry::unregister_actor(actor_name, target);
    }

    if (target->m_stop_notify_watchers) {
        notify_watchers_internal(target);
    }

    // Remove pooled actors from their worker's list. Fast actors stay listed
    // until their thread is joined (reap_stopped_fast_actors()): this may be
    // running on that very thread, and the list's shared_ptr keeps the
    // std::thread alive until it can be joined.
    if (!dynamic_cast<fast_actor*>(target)) {
        std::lock_guard<std::mutex> lock(inst.m_actors_mutex);
        size_t thread_id = target->m_assigned_thread_id;
        if (thread_id < inst.m_actors_per_thread.size()) {
            auto& thread_actors = inst.m_actors_per_thread[thread_id];
            auto it = std::find_if(thread_actors.begin(), thread_actors.end(),
                                   [target](const std::shared_ptr<actor>& ptr) {
                                       return ptr.get() == target;
                                   });
            if (it != thread_actors.end()) {
                thread_actors.erase(it);
            }
        }
    }

    // Wakes a stop_actor() blocked in m_state.wait()
    target->set_state(actor_state::stopped);
    return true;
}

void system::run_stop_to_completion(actor* target) {
    while (!target->service_stop_request()) {
        target->process_next_message();
    }
}

void system::complete_pending_stops() {
    auto& inst = instance();

    std::vector<std::shared_ptr<actor>> candidates;
    {
        std::lock_guard<std::mutex> lock(inst.m_actors_mutex);
        for (auto& thread_actors : inst.m_actors_per_thread) {
            candidates.insert(candidates.end(), thread_actors.begin(), thread_actors.end());
        }
    }
    {
        std::lock_guard<std::mutex> lock(inst.m_fast_actors_mutex);
        candidates.insert(candidates.end(), inst.m_fast_actors.begin(), inst.m_fast_actors.end());
    }

    for (auto& actor_ptr : candidates) {
        if (actor_ptr->m_stop_phase.load(std::memory_order_acquire) == actor::stop_phase::requested &&
            actor_ptr->get_state() != actor_state::stopped) {
            run_stop_to_completion(actor_ptr.get());
        }
    }

    reap_stopped_fast_actors();
}

void system::reap_stopped_fast_actors() {
    auto& inst = instance();

    // Take fast actors whose stop_actor() has completed out of the list under
    // the lock, then join their threads outside it: the thread may still be
    // unwinding from on_stop(), which is allowed to call back into the system.
    std::vector<std::shared_ptr<actor>> reaped;
    {
        std::lock_guard<std::mutex> lock(inst.m_fast_actors_mutex);
        auto split = std::stable_partition(
            inst.m_fast_actors.begin(), inst.m_fast_actors.end(),
            [](const std::shared_ptr<actor>& ptr) {
                const bool stopped_by_stop_actor =
                    ptr->m_stop_phase.load(std::memory_order_acquire) == actor::stop_phase::requested &&
                    ptr->get_state() == actor_state::stopped;
                // Never reap (and so join) the calling thread's own actor
                const bool is_this_thread =
                    static_cast<fast_actor*>(ptr.get())->m_dedicated_thread.get_id() ==
                    std::this_thread::get_id();
                return !stopped_by_stop_actor || is_this_thread;  // true = keep
            });
        reaped.assign(std::make_move_iterator(split),
                      std::make_move_iterator(inst.m_fast_actors.end()));
        inst.m_fast_actors.erase(split, inst.m_fast_actors.end());
    }

    for (auto& actor_ptr : reaped) {
        fast_actor* fast_ptr = static_cast<fast_actor*>(actor_ptr.get());
        if (!fast_ptr->m_join_claimed.exchange(true) &&
            fast_ptr->m_dedicated_thread.joinable()) {
            fast_ptr->m_dedicated_thread.join();
        }
    }
}

bool system::is_executor_of(actor* target) {
    // Self-stop from a handler (also covers an inline actor stopping itself)
    if (current_actor_context == target) {
        return true;
    }
    if (auto* fast_ptr = dynamic_cast<fast_actor*>(target)) {
        return fast_ptr->m_dedicated_thread.get_id() == std::this_thread::get_id();
    }
    // Pooled and inline actors are serviced by their assigned worker
    return t_worker_thread_id == target->m_assigned_thread_id;
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
                                 bool was_call) {
    auto& inst = instance();

    // Always increment counter (negligible overhead)
    if (was_call) {
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
