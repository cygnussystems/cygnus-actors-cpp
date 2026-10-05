# Tutorials: runnable, self-checking examples of the framework API.
#
# One binary, one tutorial per run: `tutorials 3` runs tutorial 3 and exits.
# Each tutorial is registered as a ctest test, so a tutorial that stops
# compiling OR stops working fails the build. That is the point - it makes the
# examples a standing check rather than prose that quietly goes stale.

set(TUTORIAL_SOURCES
    main.cpp
    01_hello_actor.cpp
    02_message_passing.cpp
    03_actor_to_actor.cpp
    04_lifecycle_hooks.cpp
    05_actor_registry.cpp
    06_call.cpp
    07_timers.cpp
    08_dynamic_removal.cpp
    09_watch_pattern.cpp
    10_dead_letters.cpp
    11_fixed_string.cpp
    12_message_pool.cpp
    13_shared_payload.cpp
    14_fast_actor.cpp
    15_inline_actor.cpp
    16_trading_manager.cpp
    17_stateful_actor.cpp
)

# Needs the framework built with ZeroMQ (ENABLE_ZEROMQ, vcpkg cppzmq).
# Last in the list, so its number is the same whether it is built or not.
if(ENABLE_ZEROMQ)
    list(APPEND TUTORIAL_SOURCES 18_zeromq_relay.cpp)
endif()

add_executable(tutorials ${TUTORIAL_SOURCES})

target_link_libraries(tutorials PRIVATE ${PROJECT_NAME})

target_include_directories(tutorials PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC)
    target_compile_options(tutorials PRIVATE /W4)
else()
    target_compile_options(tutorials PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Register each tutorial individually so a failure names the tutorial that
# broke rather than just "tutorials".
#
# Keep this list in step with the registry in main.cpp.
set(TUTORIAL_NAMES
    hello_actor
    message_passing
    actor_to_actor
    lifecycle_hooks
    actor_registry
    call
    timers
    dynamic_removal
    watch_pattern
    dead_letters
    fixed_string
    message_pool
    shared_payload
    fast_actor
    inline_actor
    trading_manager
    stateful_actor
)
if(ENABLE_ZEROMQ)
    list(APPEND TUTORIAL_NAMES zeromq_relay)
endif()

set(TUTORIAL_INDEX 1)
foreach(TUTORIAL_NAME IN LISTS TUTORIAL_NAMES)
    add_test(
        NAME tutorial_${TUTORIAL_INDEX}_${TUTORIAL_NAME}
        COMMAND tutorials ${TUTORIAL_INDEX}
    )
    math(EXPR TUTORIAL_INDEX "${TUTORIAL_INDEX} + 1")
endforeach()
