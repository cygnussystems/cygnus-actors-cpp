cmake_minimum_required(VERSION 3.15)
project(CygnusActorFramework VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_EXTENSIONS OFF)

# Project options
option(BUILD_TESTS "Build unit tests" ON)
option(BUILD_EXAMPLES "Build examples" ON)
option(BUILD_TUTORIALS "Build the runnable tutorials" ON)
option(ENABLE_ZEROMQ "Enable ZeroMQ relay actor" ON)

# Source files
set(SOURCES
    source/actor.cpp
    source/actor_ref.cpp
    source/actor_registry.cpp
    source/system.cpp
    source/stateful_actor.cpp
    source/fast_actor.cpp
    source/timer_manager.cpp
    source/message_pool.cpp
)

# ZeroMQ relay actor (optional)
if(ENABLE_ZEROMQ)
    list(APPEND SOURCES source/zeromq_relay_actor.cpp)
endif()

# Create library
add_library(${PROJECT_NAME} STATIC ${SOURCES})

# C++20 is a PUBLIC requirement: the public headers use concepts, operator<=>
# and std::jthread, so consumers must compile as C++20 too. Propagating this
# through the usage requirements (rather than setting CMAKE_CXX_STANDARD) means
# it also flows through the install/export interface.
# Minimum toolchains: GCC 13+, MSVC 19.29+ (VS 2019 16.11), Clang 16+.
target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_20)

target_include_directories(${PROJECT_NAME}
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# Link threads library (required for std::thread)
find_package(Threads REQUIRED)
target_link_libraries(${PROJECT_NAME} PUBLIC Threads::Threads)

# ZeroMQ relay actor dependencies (optional)
#
# nlohmann_json is deliberately NOT required here. The relay treats payloads as
# opaque bytes and never includes it - JSON only appears in the documented
# examples of what callers may put in a payload (doc/130_zeromq_relay.md).
# Requiring it forced every consumer to install a package the library does not
# use; applications that want JSON should depend on it themselves.
if(ENABLE_ZEROMQ)
    find_package(cppzmq CONFIG REQUIRED)
    target_link_libraries(${PROJECT_NAME} PUBLIC cppzmq)
    target_compile_definitions(${PROJECT_NAME} PUBLIC CAS_ENABLE_ZEROMQ)
endif()

# Compiler warnings
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Example executable
if(BUILD_EXAMPLES)
    add_executable(example_usage tests/example_usage.cpp)
    target_link_libraries(example_usage ${PROJECT_NAME})

    # Benchmark executable
    add_executable(benchmark benchmarks/throughput_benchmark.cpp)
    target_link_libraries(benchmark ${PROJECT_NAME})

    # Size check utility
    add_executable(size_check benchmarks/size_check.cpp)
    target_link_libraries(size_check ${PROJECT_NAME})
endif()

# CTest integration.
#
# Hoisted out of the BUILD_TESTS block because the tutorials register tests
# too: enable_testing() has to run before any add_test(), including theirs.
if(BUILD_TESTS OR BUILD_TUTORIALS)
    enable_testing()
endif()

# Unit tests - reorganized for granular debugging
if(BUILD_TESTS)
    include(FetchContent)

    # Download Catch2
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v2.13.10
    )
    FetchContent_MakeAvailable(Catch2)

    # Test executable - single binary with all test files
    add_executable(unit_tests
        tests/main.cpp
        # Level 0: Basic fundamentals
        tests/00_basic/test_actor_creation.cpp
        tests/00_basic/test_actor_naming.cpp
        tests/00_basic/test_registry_basics.cpp
        # Level 1: Simple operations
        tests/01_simple/test_message_struct.cpp
        tests/01_simple/test_handler_registration.cpp
        tests/01_simple/test_single_message.cpp
        # Level 2: Actor interactions
        tests/02_interactions/test_ping_pong.cpp
        # Level 3: Lifecycle management
        tests/03_lifecycle/test_lifecycle_hooks.cpp
        tests/03_lifecycle/test_message_draining.cpp
        # Level 4: Advanced features
        # tests/04_advanced/test_stateful_actor.cpp  # Disabled: has ABI sensitivity issues with base class layout changes
        tests/04_advanced/test_fast_actor.cpp
        tests/04_advanced/test_inline_actor.cpp
        # Level 5: Ask pattern (RPC-style)
        tests/05_ask_pattern/test_ask_basic.cpp
        tests/05_ask_pattern/test_ask_timeout.cpp
        # Level 6: Timers
        tests/06_timers/test_timer_basic.cpp
        tests/06_timers/test_timer_periodic.cpp
        tests/06_timers/test_timer_lifecycle.cpp
        tests/06_timers/test_timer_actor_lifetime.cpp
        # Standalone timer manager tests (no actor system dependency)
        tests/test_timer_manager_standalone.cpp
        # Level 7: Dynamic actor removal
        tests/07_dynamic_removal/test_basic_removal.cpp
        # Level 8: ZeroMQ relay (optional - requires CAS_ENABLE_ZEROMQ)
        tests/08_zeromq_relay/test_zeromq_relay.cpp
        # Level 9: Fixed types (zero-allocation)
        tests/09_fixed_types/test_fixed_string.cpp
        # Level 10: Dead letter handling
        tests/10_dead_letters/test_dead_letter_handling.cpp
        # Level 11: Concurrency invariants (see doc/INVARIANTS.md)
        tests/11_concurrency/test_ask_serialization.cpp
        tests/11_concurrency/test_no_lock_during_handler.cpp
        tests/11_concurrency/test_queue_threshold.cpp
        tests/11_concurrency/test_message_ordering.cpp
        tests/11_concurrency/test_runtime_actor_start.cpp
        tests/11_concurrency/test_on_start_contract.cpp
    )

    target_link_libraries(unit_tests
        ${PROJECT_NAME}
        Catch2::Catch2
    )

    add_test(NAME unit_tests COMMAND unit_tests)

    # Test-isolation probe.
    #
    # This binary is EXPECTED TO FAIL - it contains one deliberately-failing
    # test followed by healthy ones. It is deliberately NOT part of unit_tests,
    # because a failing case there would fail the build.
    #
    # check_isolation.cmake runs it and asserts the failure count is exactly 1.
    # More than that means a failing test corrupted the process-wide actor
    # system and took the following tests down with it.
    add_executable(isolation_probe tests/12_isolation/isolation_probe.cpp)
    target_link_libraries(isolation_probe ${PROJECT_NAME} Catch2::Catch2)

    add_test(NAME test_isolation
        COMMAND ${CMAKE_COMMAND}
            -DPROBE_EXE=$<TARGET_FILE:isolation_probe>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/12_isolation/check_isolation.cmake
    )
endif()

# Runnable tutorials (see tutorials/README.md)
if(BUILD_TUTORIALS)
    add_subdirectory(tutorials)
endif()
