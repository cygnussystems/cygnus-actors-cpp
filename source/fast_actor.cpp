#include "cas/fast_actor.h"
#include "cas/message_base.h"

// Cross-platform CPU pause instruction
#if defined(_MSC_VER)
    #include <intrin.h>
    #define CPU_PAUSE() _mm_pause()
#elif defined(__GNUC__) || defined(__clang__)
    #if defined(__x86_64__) || defined(__i386__)
        #define CPU_PAUSE() __builtin_ia32_pause()
    #elif defined(__aarch64__) || defined(__arm__)
        #define CPU_PAUSE() __asm__ __volatile__("yield")
    #else
        #define CPU_PAUSE() do {} while(0)
    #endif
#else
    #define CPU_PAUSE() do {} while(0)
#endif

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
    CPU_PAUSE();
}

void fast_actor::run_dedicated_thread() {
    // Call on_start() for initialization
    current_actor_context = this;
    on_start();
    current_actor_context = nullptr;

    // Tight polling loop with configurable strategy
    while (get_state() == actor_state::running) {
        if (has_messages()) {
            current_actor_context = this;
            process_next_message();
            current_actor_context = nullptr;
        } else {
            // Apply polling strategy when no messages
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
    }

    // Drain remaining messages during shutdown
    while (has_messages()) {
        current_actor_context = this;
        process_next_message();
        current_actor_context = nullptr;
    }

    // Call on_stop()
    current_actor_context = this;
    on_stop();
    current_actor_context = nullptr;
}

} // namespace cas
