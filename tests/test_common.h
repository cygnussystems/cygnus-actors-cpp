#ifndef TEST_COMMON_H
#define TEST_COMMON_H

#include <catch2/catch.hpp>
#include "cas/cas.h"
#include <thread>
#include <chrono>
#include <atomic>

// Helper to ensure clean state for each test
#define TEST_CLEANUP() \
    if (cas::system::is_running()) { \
        cas::system::shutdown(); \
        cas::system::wait_for_shutdown(); \
    } \
    cas::system::reset()

// Helper for common wait pattern
inline void wait_ms(int milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

#endif // TEST_COMMON_H
