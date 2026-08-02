#ifndef TEST_COMMON_H
#define TEST_COMMON_H

#include <catch2/catch.hpp>
#include "cas/cas.h"
#include <thread>
#include <chrono>
#include <atomic>

// Shut the actor system down and reset it to a pristine state.
// Safe to call when the system is not running.
inline void cas_test_reset() noexcept {
    try {
        if (cas::system::is_running()) {
            cas::system::shutdown();
            cas::system::wait_for_shutdown();
        }
        cas::system::reset();
    } catch (...) {
        // Never propagate: this runs from a destructor during stack unwinding
        // when a REQUIRE has already failed. Throwing here would terminate.
    }
}

// RAII guard that resets the actor system when the enclosing scope exits.
//
// IMPORTANT: declare this at the TOP of any test that starts the system.
//
//     TEST_CASE("...") {
//         CAS_TEST_GUARD();
//         ...
//     }
//
// A trailing TEST_CLEANUP() call is NOT sufficient on its own. Catch2's
// REQUIRE throws on failure, so a failing assertion unwinds past any cleanup
// at the end of the test body. Because the actor system is a process-wide
// singleton, that leaves it running and every subsequent test fails too:
// start() returns early when already running, so handlers never register.
// One real failure then produces a cascade of fake ones.
struct cas_test_guard {
    cas_test_guard() = default;
    ~cas_test_guard() { cas_test_reset(); }

    cas_test_guard(const cas_test_guard&) = delete;
    cas_test_guard& operator=(const cas_test_guard&) = delete;
};

// Two levels of indirection so __LINE__ expands before pasting
#define CAS_GUARD_CAT_(a, b) a##b
#define CAS_GUARD_NAME_(a, b) CAS_GUARD_CAT_(a, b)
#define CAS_TEST_GUARD() \
    cas_test_guard CAS_GUARD_NAME_(cas_guard_, __LINE__)

// RAII guard for tests that change global system_config or install global
// handlers. Restores defaults on scope exit, including when a REQUIRE throws.
// Resets the system first, since configure() rejects a running system.
struct cas_config_guard {
    cas_config_guard() = default;

    ~cas_config_guard() {
        cas_test_reset();
        try {
            cas::system::clear_queue_threshold_handler();
            cas::system::clear_dead_letter_handler();
            cas::system::configure(cas::system_config{});
        } catch (...) {
            // Never propagate from a destructor
        }
    }

    cas_config_guard(const cas_config_guard&) = delete;
    cas_config_guard& operator=(const cas_config_guard&) = delete;
};

#define CAS_CONFIG_GUARD() \
    cas_config_guard CAS_GUARD_NAME_(cas_cfg_guard_, __LINE__)

// Explicit mid-test or end-of-test reset.
// Still useful when a test needs the system torn down before it finishes
// (e.g. to call system::configure(), which rejects a running system).
// Idempotent, and harmless alongside CAS_TEST_GUARD().
#define TEST_CLEANUP() cas_test_reset()

// Helper for common wait pattern
inline void wait_ms(int milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

#endif // TEST_COMMON_H
