#ifndef CAS_SOURCE_CPU_PAUSE_H
#define CAS_SOURCE_CPU_PAUSE_H

// Internal (not installed): a CPU hint for spin-wait loops. Tells the core we
// are polling, which saves power and frees resources for a sibling
// hyperthread; it is not a sleep and does not yield to the OS.

#if defined(_MSC_VER)
    #include <intrin.h>
    #define CAS_CPU_PAUSE() _mm_pause()
#elif defined(__GNUC__) || defined(__clang__)
    #if defined(__x86_64__) || defined(__i386__)
        #define CAS_CPU_PAUSE() __builtin_ia32_pause()
    #elif defined(__aarch64__) || defined(__arm__)
        #define CAS_CPU_PAUSE() __asm__ __volatile__("yield")
    #else
        #define CAS_CPU_PAUSE() do {} while (0)
    #endif
#else
    #define CAS_CPU_PAUSE() do {} while (0)
#endif

#endif // CAS_SOURCE_CPU_PAUSE_H
