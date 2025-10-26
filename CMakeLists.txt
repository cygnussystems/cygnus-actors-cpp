cmake_minimum_required(VERSION 3.15)
project(CygnusActorFramework VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Project options
option(BUILD_TESTS "Build unit tests" ON)
option(BUILD_EXAMPLES "Build examples" ON)

# Source files
set(SOURCES
    source/actor.cpp
    source/actor_ref.cpp
    source/actor_registry.cpp
    source/system.cpp
    source/stateful_actor.cpp
    source/fast_actor.cpp
)

# Create library
add_library(${PROJECT_NAME} STATIC ${SOURCES})

target_include_directories(${PROJECT_NAME}
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# Link threads library (required for std::thread)
find_package(Threads REQUIRED)
target_link_libraries(${PROJECT_NAME} PUBLIC Threads::Threads)

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
        tests/04_advanced/test_stateful_actor.cpp
        tests/04_advanced/test_fast_actor.cpp
        tests/04_advanced/test_inline_actor.cpp
    )

    target_link_libraries(unit_tests
        ${PROJECT_NAME}
        Catch2::Catch2
    )

    # Enable CTest integration
    enable_testing()
    add_test(NAME unit_tests COMMAND unit_tests)
endif()
