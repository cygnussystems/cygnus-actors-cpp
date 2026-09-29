#ifndef CAS_TUTORIALS_H
#define CAS_TUTORIALS_H

#include <atomic>
#include <chrono>
#include <string>

// Shared declarations for the tutorial programs.
//
// Each tutorial lives in its own file and exposes a single run() function.
// main.cpp looks one up by number and calls it - exactly one tutorial runs
// per process, then the process exits.
//
// That one-per-process rule is not incidental. The actor system is a
// process-wide singleton, so running two tutorials back to back in one
// process would let the first one's state leak into the second. Keeping each
// run in a fresh process means every tutorial starts from a pristine system,
// and no tutorial has to carry teardown code that would distract from what it
// is teaching.

namespace tut {

// Each tutorial returns true on success, false if a check failed.
//
// Tutorials verify their own behaviour rather than just printing. A tutorial
// that prints the right thing and then deadlocks looks identical to a working
// one in a CI log; returning a result makes ctest catch documentation rot,
// not merely compilation failure.
using tutorial_fn = bool (*)();

struct tutorial_entry {
    int number;
    const char* name;
    const char* description;
    tutorial_fn run;
};

// Tutorial entry points, in teaching order.
bool run_hello_actor();       // 1
bool run_message_passing();   // 2
bool run_actor_to_actor();    // 3
bool run_lifecycle_hooks();   // 4
bool run_actor_registry();    // 5
bool run_call();       // 6
bool run_timers();            // 7
bool run_dynamic_removal();   // 8
bool run_watch_pattern();     // 9
bool run_dead_letters();      // 10
bool run_fixed_string();      // 11
bool run_message_pool();      // 12
bool run_fast_actor();        // 13
bool run_inline_actor();      // 14
bool run_trading_manager();   // 15
bool run_stateful_actor();    // 16
#ifdef CAS_ENABLE_ZEROMQ
bool run_zeromq_relay();      // 17 - only with ZeroMQ
#endif

// The registry. Defined in main.cpp.
extern const tutorial_entry g_tutorials[];
extern const int g_tutorial_count;

// ---------------------------------------------------------------------------
// Small helpers shared by the tutorials.
//
// These exist so tutorial code stays focused on the framework rather than on
// scaffolding. They are NOT part of the framework API.
// ---------------------------------------------------------------------------

// Prints an underlined heading, so the console output of a tutorial maps
// onto the steps of its source file.
void heading(const std::string& title);

// Reports a single check. Prints a pass/fail line and returns the condition,
// so tutorials can accumulate a result without early-exiting.
bool check(bool condition, const std::string& what);

// Waiting for an actor.
//
// Sending a message does not block, so a tutorial that wants to observe the
// result has to wait for the actor's thread to get there. These poll for the
// actual condition - rather than sleeping a fixed time - so the tutorials are
// neither flaky on a loaded machine nor slow on a fast one. Each returns true
// once the condition holds, false if the timeout passes first.
//
//   wait_until(actor::greeter::s_greeting_count, 1)   a counter reaches a value
//   wait_until(actor::exchange::s_bound)              a flag is set
//   wait_until(&actor::session::order_count, 3)       a function returns a value
//   wait_until(&dead_letter_log::any_dropped)         a function returns true

namespace detail {
std::chrono::steady_clock::time_point deadline_after(int timeout_ms);
bool expired(std::chrono::steady_clock::time_point deadline);  // sleeps a moment first
}

bool wait_until(bool (*condition)(), int timeout_ms = 2000);
bool wait_until(const std::atomic<bool>& flag, int timeout_ms = 2000);

template<typename T, typename U>
bool wait_until(const std::atomic<T>& value, U expected, int timeout_ms = 2000) {
    const auto deadline = detail::deadline_after(timeout_ms);
    while (value.load() != static_cast<T>(expected)) {
        if (detail::expired(deadline)) {
            return value.load() == static_cast<T>(expected);
        }
    }
    return true;
}

template<typename T, typename U>
bool wait_until(T (*getter)(), U expected, int timeout_ms = 2000) {
    const auto deadline = detail::deadline_after(timeout_ms);
    while (getter() != static_cast<T>(expected)) {
        if (detail::expired(deadline)) {
            return getter() == static_cast<T>(expected);
        }
    }
    return true;
}

} // namespace tut

#endif // CAS_TUTORIALS_H
