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
     "Several message types, member-function and lambda handlers.",
     run_message_passing},
    {3, "actor_to_actor",
     "Actors messaging each other, and replying via the sender.",
     run_actor_to_actor},
};

const int g_tutorial_count =
    static_cast<int>(sizeof(g_tutorials) / sizeof(g_tutorials[0]));

void section(const std::string& title) {
    std::cout << "\n--- " << title << " ---\n";
}

bool check(bool condition, const std::string& what) {
    std::cout << (condition ? "  [ok]   " : "  [FAIL] ") << what << "\n";
    return condition;
}

bool wait_until(const std::function<bool()>& done, int timeout_ms) {
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);
    while (clock::now() < deadline) {
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // One final check, so a condition satisfied right at the deadline still
    // counts rather than being reported as a timeout.
    return done();
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
