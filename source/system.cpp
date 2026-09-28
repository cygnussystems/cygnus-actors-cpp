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
#include <string_view>
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
    // Actors will call on_shutdown() and set state=stopping when they process this message.
    //
    // INVARIANT 2: enqueueing can run user code on this thread - an
    // inline_actor dispatches system_shutdown (so its on_shutdown()) right
    // here, and any enqueue may fire the queue-threshold or dead-letter
    // callback. So work on snapshots, never under the list mutexes: user code
    // that calls back into the system (actor_count(), create(), ...) would
    // re-take them and deadlock.
    for (const auto& list : {snapshot_pooled_actors(), snapshot_fast_actors()}) {
        for (const auto& actor_ptr : list) {
            // An actor mid-stop_actor() is already stopping; sending it
            // system_shutdown would only register a spurious dead letter.
            if (actor_ptr->get_state() != actor_state::running) continue;
            actor_ptr->enqueue_message(std::make_unique<system_shutdown>());
        }
    }

    // Phase 2: Monitor queue draining with deadline
    auto start_time = std::chrono::steady_clock::now();
    auto deadline = start_time + config.drain_timeout;

    while (std::chrono::steady_clock::now() < deadline) {
        // has_messages() is virtual: checked on snapshots, not under the locks.
        bool all_empty = true;
        for (const auto& list : {snapshot_pooled_actors(), snapshot_fast_actors()}) {
            for (const auto& actor_ptr : list) {
                if (actor_ptr->has_messages()) {
                    all_empty = false;
                    break;
                }
            }
            if (!all_empty) break;
        }

        if (all_empty) {
            break;  // All queues drained successfully
        }

        std::this_thread::sleep_for(config.check_interval);
    }

    // Phase 3: Check for undrained actors and log warnings (on snapshots:
    // queue_size() is virtual)
    auto log_undrained = [&inst](const std::vector<std::shared_ptr<actor>>& list,
                                 const char* kind) {
        for (const auto& actor_ptr : list) {
            size_t remaining = actor_ptr->queue_size();
            if (remaining > 0) {
                std::string warning = std::string(kind) + " '" + actor_ptr->name() +
                                      "' has " + std::to_string(remaining) +
                                      " unprocessed messages at shutdown";
                std::lock_guard<std::mutex> log_lock(inst.m_shutdown_log_mutex);
                inst.m_shutdown_log.push_back(warning);
            }
        }
    };
    log_undrained(snapshot_pooled_actors(), "Actor");
    log_undrained(snapshot_fast_actors(), "Fast actor");

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

    // Call on_stop on all pooled actors and mark them as stopped.
    // INVARIANT 2: on_stop() is user code and may call back into the system
    // (actor_count(), create(), ...), which takes m_actors_mutex - so never
    // call it with that mutex held. The workers are joined, so this thread is
    // the only one touching the actors.
    for (const auto& actor_ptr : snapshot_pooled_actors()) {
        // Cancel any active timers for this actor
        cancel_actor_timers(actor_ptr.get());

        actor_ptr->run_stop_hook(actor::stop_hook::on_stop);
        actor_ptr->set_state(actor_state::stopped);
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

    // Forget all watches. Instance ids restart below, so a stale entry would
    // attach an old watcher to a new actor that gets the same id.
    {
        std::lock_guard<std::mutex> lock(inst.m_watchers_mutex);
        inst.m_watchers.clear();
        inst.m_watching.clear();
    }

    // Reset instance ID counter
    inst.m_next_instance_id.store(1);

    inst.m_handler_exceptions.store(0);

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

    // Default name now, before the actor is shared: name() is read from
    // other threads and must never be written lazily (issue 009)
    actor_ptr->assign_default_name();

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
        try {
            on_start_scope scope(actor_ptr.get());
            actor_ptr->on_start();
        } catch (...) {
            // The caller of create() gets the exception; the half-created
            // actor must not stay behind with a mailbox nobody will read
            // (it would hold every later shutdown for its drain timeout).
            discard_unstarted_actor(actor_ptr.get());
            throw;
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

    // A stopping actor accepts no messages, so every tick would be a dead
    // letter: don't schedule (issue 013). Covers handlers still draining
    // after on_shutdown() during system shutdown.
    if (target->get_state() != actor_state::running) {
        return INVALID_TIMER_ID;
    }

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
            // Each firing is a new message; timer messages have no sender
            fired_msg->message_id = next_message_id();
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

void system::forget_finished_timers(std::set<timer_id>& ids) {
    instance().m_timer_manager.retain_active(ids);
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
        // Runs on the sending thread - possibly the timer thread or a worker
        // outside any handler (INVARIANT 7). Zero cost unless it throws.
        try {
            (*handler)(info);
        } catch (...) {
            report_handler_exception(actor_name, typeid(void), 0, "queue_threshold_handler",
                                     std::current_exception(), exception_action::resume);
        }
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

    // Final hooks, on this - the executing - thread. An exception from
    // either is reported; the stop still completes.
    target->run_stop_hook(actor::stop_hook::on_shutdown);
    target->run_stop_hook(actor::stop_hook::on_stop);

    // on_shutdown() may have scheduled timers; the actor is gone, cancel them.
    cancel_actor_timers(target);

    // Close the mailbox. From here a push fails atomically instead of landing
    // in a mailbox nobody will read: before, a tell() that passed the running
    // check just as the stop completed was silently lost. Anything still queued
    // - left over after a drain timeout, or pushed during the stop - is a dead
    // letter. (A pending system_shutdown is control traffic, not a message.)
    // First let the actor type drop what only it knows about - a stateful
    // actor's held messages - so every dead letter is reported before the
    // actor is marked stopped and a waiting stop_actor() returns.
    target->discard_pending_messages();
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

    // Notify watchers (if requested), and in any case remove every watch this
    // actor is part of - as watched and as watcher.
    end_watches(target, target->m_stop_notify_watchers);

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

void system::discard_unstarted_actor(actor* target) {
    auto& inst = instance();

    target->set_state(actor_state::stopped);
    cancel_actor_timers(target);
    target->m_mailbox.close([target](message_base& msg, bool is_call) {
        report_dead_letter(target->name(), &msg, actor_state::stopped, is_call);
    });
    actor_registry::unregister_actor(target->name(), target);
    end_watches(target, false);

    if (dynamic_cast<fast_actor*>(target)) {
        // No thread was launched for it
        std::lock_guard<std::mutex> lock(inst.m_fast_actors_mutex);
        std::erase_if(inst.m_fast_actors,
                      [target](const std::shared_ptr<actor>& ptr) { return ptr.get() == target; });
    } else {
        std::lock_guard<std::mutex> lock(inst.m_actors_mutex);
        const size_t thread_id = target->m_assigned_thread_id;
        if (thread_id < inst.m_actors_per_thread.size()) {
            std::erase_if(inst.m_actors_per_thread[thread_id],
                          [target](const std::shared_ptr<actor>& ptr) { return ptr.get() == target; });
        }
    }
}

void system::run_stop_to_completion(actor* target) {
    while (!target->service_stop_request()) {
        target->process_next_message();
    }
}

std::vector<std::shared_ptr<actor>> system::snapshot_pooled_actors() {
    auto& inst = instance();
    std::vector<std::shared_ptr<actor>> copy;
    std::lock_guard<std::mutex> lock(inst.m_actors_mutex);
    for (const auto& thread_actors : inst.m_actors_per_thread) {
        copy.insert(copy.end(), thread_actors.begin(), thread_actors.end());
    }
    return copy;
}

std::vector<std::shared_ptr<actor>> system::snapshot_fast_actors() {
    auto& inst = instance();
    std::lock_guard<std::mutex> lock(inst.m_fast_actors_mutex);
    return inst.m_fast_actors;
}

void system::complete_pending_stops() {
    std::vector<std::shared_ptr<actor>> candidates = snapshot_pooled_actors();
    const auto fast = snapshot_fast_actors();
    candidates.insert(candidates.end(), fast.begin(), fast.end());

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

void system::check_call_allowed(actor* target) {
    actor* current = current_actor_context;

    // call() during on_start() would block waiting for a reply from a system
    // that is not necessarily processing messages yet - at startup no worker
    // thread has been launched. Reject it rather than hang.
    if (current && current->is_initialising()) {
        throw on_start_violation(
            "call() is not permitted from on_start(): actor '" + current->name() +
            "' is still initialising and no worker is guaranteed to service the "
            "request. Use tell() and handle the reply in a message handler.");
    }
    if (current == target && current) {
        throw call_deadlock_error(
            "call() would deadlock: actor '" + target->name() +
            "' cannot call itself. Call the method directly instead.");
    }
    // An inline actor handles the call on this thread before call() returns.
    // (Re-entering one that is already handling a message on this thread is
    // refused by inline_actor itself.)
    if (target->runs_inline()) {
        return;
    }
    // Blocking the only thread that executes the target means the request is
    // never serviced. Decided by thread, not by the calling actor: an inline
    // actor's handler runs on its sender's thread, whatever its affinity.
    if (is_executor_of(target)) {
        throw call_deadlock_error(
            "call() would deadlock: target actor '" + target->name() +
            "' shares a worker thread with the caller" +
            (current ? " '" + current->name() + "'" : std::string()) +
            ". Use tell() with an explicit reply message instead.");
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

    std::shared_ptr<actor> watcher_ptr = watcher.get_actor();
    actor* watched_ptr = watched.get<actor>();
    if (!watcher_ptr || !watched_ptr) {
        return;  // Invalid reference
    }
    // A stopped actor will never stop again, and a stopped watcher would never
    // be told: registering either would only leave an entry nobody removes.
    if (watched_ptr->get_state() == actor_state::stopped ||
        watcher_ptr->get_state() == actor_state::stopped) {
        return;
    }

    const size_t watcher_id = watcher_ptr->instance_id();
    const size_t watched_id = watched_ptr->instance_id();
    std::lock_guard<std::mutex> lock(inst.m_watchers_mutex);
    inst.m_watchers[watched_id][watcher_id] = watcher_ptr;  // a repeat watch is one entry
    inst.m_watching[watcher_id].insert(watched_id);
}

void system::unwatch(actor_ref watcher, actor_ref watched) {
    auto& inst = instance();

    actor* watcher_ptr = watcher.get<actor>();
    actor* watched_ptr = watched.get<actor>();
    if (!watcher_ptr || !watched_ptr) {
        return;  // Invalid reference
    }

    const size_t watcher_id = watcher_ptr->instance_id();
    const size_t watched_id = watched_ptr->instance_id();
    std::lock_guard<std::mutex> lock(inst.m_watchers_mutex);

    auto it = inst.m_watchers.find(watched_id);
    if (it != inst.m_watchers.end()) {
        it->second.erase(watcher_id);
        if (it->second.empty()) inst.m_watchers.erase(it);
    }
    auto rit = inst.m_watching.find(watcher_id);
    if (rit != inst.m_watching.end()) {
        rit->second.erase(watched_id);
        if (rit->second.empty()) inst.m_watching.erase(rit);
    }
}

void system::notify_watchers_internal(actor* stopped_actor) {
    end_watches(stopped_actor, true);
}

void system::end_watches(actor* stopped_actor, bool notify) {
    auto& inst = instance();
    const size_t id = stopped_actor->instance_id();

    // Under the lock: take this actor's watchers out of the table, and remove
    // it as a watcher of everything it watched. Send only after releasing it
    // (INVARIANT 2): tell() can run user code, e.g. a dead-letter handler.
    std::vector<std::shared_ptr<actor>> to_notify;
    {
        std::lock_guard<std::mutex> lock(inst.m_watchers_mutex);

        auto it = inst.m_watchers.find(id);
        if (it != inst.m_watchers.end()) {
            for (auto& [watcher_id, weak_watcher] : it->second) {
                // The reverse entry goes either way
                auto rit = inst.m_watching.find(watcher_id);
                if (rit != inst.m_watching.end()) {
                    rit->second.erase(id);
                    if (rit->second.empty()) inst.m_watching.erase(rit);
                }
                if (auto watcher = weak_watcher.lock()) {
                    to_notify.push_back(std::move(watcher));
                }
                // else: destroyed before this actor stopped - nothing to tell
            }
            inst.m_watchers.erase(it);
        }

        auto wit = inst.m_watching.find(id);
        if (wit != inst.m_watching.end()) {
            for (size_t watched_id : wit->second) {
                auto entry = inst.m_watchers.find(watched_id);
                if (entry != inst.m_watchers.end()) {
                    entry->second.erase(id);
                    if (entry->second.empty()) inst.m_watchers.erase(entry);
                }
            }
            inst.m_watching.erase(wit);
        }
    }

    if (!notify) {
        return;
    }
    for (const auto& watcher : to_notify) {
        // A watcher that has stopped (but is still referenced somewhere) would
        // only turn the notification into a dead letter.
        if (watcher->get_state() != actor_state::running) {
            continue;
        }
        termination_msg msg;
        msg.actor_name = stopped_actor->name();
        msg.instance_id = id;
        msg.type_name = stopped_actor->type_name();
        actor_ref(watcher).tell(msg);
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

void system::set_handler_exception_handler(handler_exception_handler handler) {
    auto& inst = instance();
    std::lock_guard<std::mutex> lock(inst.m_handler_exception_handler_mutex);
    if (handler) {
        inst.m_handler_exception_handler = std::make_shared<handler_exception_handler>(std::move(handler));
    } else {
        inst.m_handler_exception_handler.reset();
    }
}

void system::clear_handler_exception_handler() {
    auto& inst = instance();
    std::lock_guard<std::mutex> lock(inst.m_handler_exception_handler_mutex);
    inst.m_handler_exception_handler.reset();
}

size_t system::handler_exception_count() {
    return instance().m_handler_exceptions.load(std::memory_order_relaxed);
}

void system::report_handler_exception(const actor& target, std::type_index message_type,
                                      uint64_t message_id, const char* source,
                                      std::exception_ptr error, exception_action action) {
    report_handler_exception(target.name(), message_type, message_id, source, std::move(error), action);
}

void system::report_handler_exception(const std::string& actor_name, std::type_index message_type,
                                      uint64_t message_id, const char* source,
                                      std::exception_ptr error, exception_action action) {
    auto& inst = instance();
    inst.m_handler_exceptions.fetch_add(1, std::memory_order_relaxed);

    std::string what = "unknown exception";
    try {
        std::rethrow_exception(error);
    } catch (const std::exception& e) {
        what = e.what();
    } catch (...) {
    }

    // INVARIANT 2: copy the callback under its mutex, call it without
    std::shared_ptr<handler_exception_handler> handler;
    {
        std::lock_guard<std::mutex> lock(inst.m_handler_exception_handler_mutex);
        handler = inst.m_handler_exception_handler;
    }

    if (handler && *handler) {
        handler_exception_info info(actor_name, message_type, message_id, source,
                                    what, error, action);
        try {
            (*handler)(info);
            return;
        } catch (...) {
            // A throwing reporting callback must not take the thread down
            // either; fall through so the original exception is not lost
            std::cerr << "[CAS] the handler-exception callback threw\n";
        }
    }

    // No callback: never lose it silently (INVARIANT 5)
    std::cerr << "[CAS] exception in " << source << " of actor '" << actor_name << "'";
    if (message_type != std::type_index(typeid(void))) {
        std::cerr << " handling " << message_type.name();
    }
    const bool fatal = std::string_view(source) == "on_start";  // fast_actor created before start()
    std::cerr << ": " << what
              << (fatal ? " - fatal, terminating"
                        : action == exception_action::stop ? " - stopping the actor" : "")
              << "\n";
}

void system::report_invalid_target(std::type_index message_type) {
    auto& inst = instance();
    inst.m_dead_letter_invalid.fetch_add(1, std::memory_order_relaxed);

    if (inst.m_config.log_dead_letters) {
        std::cerr << "[DEAD LETTER] " << message_type.name()
                  << " sent to an invalid actor_ref\n";
    }

    std::shared_ptr<dead_letter_handler> handler;
    {
        std::lock_guard<std::mutex> lock(inst.m_dead_letter_handler_mutex);
        handler = inst.m_dead_letter_handler;
    }
    if (handler && *handler) {
        dead_letter_info info(std::string(), message_type, 0, actor_state::stopped);
        try {
            (*handler)(info);
        } catch (...) {
            report_handler_exception(std::string(), message_type, 0, "dead_letter_handler",
                                     std::current_exception(), exception_action::resume);
        }
    }
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
        // Runs wherever the message was dropped: a sender's thread, the timer
        // thread, or a worker discarding a mailbox (INVARIANT 7)
        try {
            (*handler)(info);
        } catch (...) {
            report_handler_exception(actor_name, typeid(*msg), msg->message_id, "dead_letter_handler",
                                     std::current_exception(), exception_action::resume);
        }
    }
}

} // namespace cas
