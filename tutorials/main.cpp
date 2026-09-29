// Tutorial dispatcher.
//
// Usage:
//   tutorials          - list the available tutorials
//   tutorials <n>      - run tutorial <n> and exit
//
// Exit code is 0 when the tutorial's own checks all passed, 1 otherwise, so
// each tutorial can be registered as a ctest test and doc rot fails the build.

#include "tutorials.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace tut {

const tutorial_entry g_tutorials[] = {
    {1, "hello_actor",
     "Define a message and an actor, send it one message.",
     run_hello_actor},
    {2, "message_passing",
     "Several message types, and state that needs no locks.",
     run_message_passing},
    {3, "actor_to_actor",
     "Actors messaging each other, and replying via the sender.",
     run_actor_to_actor},
    {4, "lifecycle_hooks",
     "on_start / on_shutdown / on_stop, and message draining.",
     run_lifecycle_hooks},
    {5, "actor_registry",
     "Finding actors by name instead of holding a reference.",
     run_actor_registry},
    {6, "call",
     "Request/response with a return value, timeouts, deadlock rules.",
     run_call},
    {7, "timers",
     "One-shot and periodic timers, and cancelling them.",
     run_timers},
    {8, "dynamic_removal",
     "Stopping one actor while the system keeps running.",
     run_dynamic_removal},
    {9, "watch_pattern",
     "Being notified when a watched actor terminates.",
     run_watch_pattern},
    {10, "dead_letters",
     "Seeing the messages that could not be delivered.",
     run_dead_letters},
    {11, "fixed_string",
     "Inline strings, for messages that do not allocate.",
     run_fixed_string},
    {12, "message_pool",
     "How messages are allocated, and how to keep them on the fast path.",
     run_message_pool},
    {13, "fast_actor",
     "An actor with a dedicated thread, for latency-critical work.",
     run_fast_actor},
    {14, "inline_actor",
     "Synchronous dispatch in the sender's thread.",
     run_inline_actor},
    {15, "trading_manager",
     "Launching and retiring actors at runtime: a manager, its trading systems and their streams.",
     run_trading_manager},
    {16, "stateful_actor",
     "Selective receive: an exchange session that holds orders until it is logged in.",
     run_stateful_actor},
#ifdef CAS_ENABLE_ZEROMQ
    {17, "zeromq_relay",
     "Talking to other processes: relays, routing, request/reply, errors.",
     run_zeromq_relay},
#endif
};

const int g_tutorial_count =
    static_cast<int>(sizeof(g_tutorials) / sizeof(g_tutorials[0]));

void heading(const std::string& title) {
    std::cout << "\n" << title << "\n" << std::string(title.size(), '-') << "\n";
}

bool check(bool condition, const std::string& what) {
    std::cout << (condition ? "  [ok]   " : "  [FAIL] ") << what << "\n";
    return condition;
}

namespace detail {

std::chrono::steady_clock::time_point deadline_after(int timeout_ms) {
    return std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
}

bool expired(std::chrono::steady_clock::time_point deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return std::chrono::steady_clock::now() >= deadline;
}

} // namespace detail

bool wait_until(bool (*condition)(), int timeout_ms) {
    const auto deadline = detail::deadline_after(timeout_ms);
    while (!condition()) {
        if (detail::expired(deadline)) {
            return condition();  // one last look, so a late success still counts
        }
    }
    return true;
}

bool wait_until(const std::atomic<bool>& flag, int timeout_ms) {
    const auto deadline = detail::deadline_after(timeout_ms);
    while (!flag.load()) {
        if (detail::expired(deadline)) {
            return flag.load();
        }
    }
    return true;
}

} // namespace tut

namespace {

void print_usage() {
    std::cout << "Cygnus Actor Framework - tutorials\n\n"
              << "Each tutorial is a complete, runnable program. Read the\n"
              << "source alongside the output: tutorials/<nn>_<name>.cpp\n\n"
              << "Usage:\n"
              << "  tutorials          list tutorials\n"
              << "  tutorials <n>      run tutorial <n>\n\n"
              << "Available tutorials:\n";

    for (int i = 0; i < tut::g_tutorial_count; ++i) {
        const auto& t = tut::g_tutorials[i];
        std::cout << "  " << t.number << ". " << t.name << "\n"
                  << "     " << t.description << "\n";
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 0;
    }

    const std::string arg = argv[1];
    if (arg == "-h" || arg == "--help") {
        print_usage();
        return 0;
    }

    int requested = 0;
    try {
        requested = std::stoi(arg);
    } catch (const std::exception&) {
        std::cerr << "Not a tutorial number: " << arg << "\n\n";
        print_usage();
        return 1;
    }

    for (int i = 0; i < tut::g_tutorial_count; ++i) {
        const auto& t = tut::g_tutorials[i];
        if (t.number != requested) {
            continue;
        }

        std::cout << "=== Tutorial " << t.number << ": " << t.name << " ===\n"
                  << t.description << "\n";

        const bool ok = t.run();

        std::cout << "\n=== Tutorial " << t.number << ": "
                  << (ok ? "PASSED" : "FAILED") << " ===\n";
        return ok ? 0 : 1;
    }

    std::cerr << "No such tutorial: " << requested << "\n\n";
    print_usage();
    return 1;
}
