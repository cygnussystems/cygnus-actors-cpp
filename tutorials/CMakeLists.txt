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
)

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
)

set(TUTORIAL_INDEX 1)
foreach(TUTORIAL_NAME IN LISTS TUTORIAL_NAMES)
    add_test(
        NAME tutorial_${TUTORIAL_INDEX}_${TUTORIAL_NAME}
        COMMAND tutorials ${TUTORIAL_INDEX}
    )
    math(EXPR TUTORIAL_INDEX "${TUTORIAL_INDEX} + 1")
endforeach()
