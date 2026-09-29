#include "cas/fast_actor.h"
#include <exception>
#include "cas/system.h"
#include "cas/message_base.h"

#include "cpu_pause.h"

namespace cas {

fast_actor::fast_actor(polling_strategy strategy)
    : m_polling_strategy(strategy) {
}

void fast_actor::set_polling_strategy(polling_strategy strategy) {
    m_polling_strategy = strategy;
}

void fast_actor::set_spin_count(int count) {
    m_spin_count = count;
}

void fast_actor::cpu_pause() {
    CAS_CPU_PAUSE();
}

void fast_actor::run_dedicated_thread() {
    // Call on_start() for initialization
    {
        try {
            run_on_start();
        } catch (...) {
            // Nobody to hand it to on this thread (for other actors the
            // caller of start()/create() gets it). A failed on_start() is a
            // programming error: report why, then terminate as before.
            system::report_handler_exception(*this, typeid(void), 0, "on_start",
                                             std::current_exception(), exception_action::stop);
            std::terminate();
        }
    }

    run_dedicated_thread_started();
}

void fast_actor::run_dedicated_thread_started() {
    // Entry point for actors whose on_start() has already been called by
    // register_actor() (created after the system was already running).

    // Tight polling loop with configurable strategy
    while (true) {
        const stop_phase phase = current_stop_phase();

        if (phase == stop_phase::requested) {
            // stop_actor(): the stop - including on_shutdown() and on_stop() -
            // is carried out here, on this thread (INVARIANT 1).
            if (service_stop_request()) {
                return;  // Complete; on_stop() has run. Thread is joined by the system.
            }
            process_next_message();  // Still draining
            continue;
        }

        if (phase == stop_phase::system_shutdown) {
            break;  // system_shutdown dispatched on this thread: drain and stop below
        }

        if (has_messages()) {
            current_actor_context = this;
            process_next_message();
            current_actor_context = nullptr;
        } else {
            wait_for_work();
        }
    }

    // Drain remaining messages during shutdown
    while (has_messages()) {
        current_actor_context = this;
        process_next_message();
        current_actor_context = nullptr;
    }

    // Call on_stop(); an exception is reported, not propagated
    run_stop_hook(stop_hook::on_stop);
}

void fast_actor::wake() {
}

void fast_actor::wait_for_work() {
    // One step of the polling strategy; the loop then checks again
    switch (m_polling_strategy) {
        case polling_strategy::yield:
            // Yield to scheduler - cooperative, low latency
            std::this_thread::yield();
            break;

        case polling_strategy::hybrid:
            // Spin briefly, then yield
            for (int i = 0; i < m_spin_count; ++i) {
                if (has_messages()) break;
                cpu_pause();
            }
            if (!has_messages()) {
                std::this_thread::yield();
            }
            break;

        case polling_strategy::busy_wait:
            // Pure busy-wait - minimum latency, maximum CPU
            cpu_pause();
            break;
    }
}

} // namespace cas
