#ifndef CAS_TUTORIALS_H
#define CAS_TUTORIALS_H

#include <functional>
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

// The registry. Defined in main.cpp.
extern const tutorial_entry g_tutorials[];
extern const int g_tutorial_count;

// ---------------------------------------------------------------------------
// Small helpers shared by the tutorials.
//
// These exist so tutorial code stays focused on the framework rather than on
// scaffolding. They are NOT part of the framework API.
// ---------------------------------------------------------------------------

// Prints a labelled section header, so the console output of a tutorial maps
// onto the sections of its source file.
void section(const std::string& title);

// Reports a single check. Prints a pass/fail line and returns the condition,
// so tutorials can accumulate a result without early-exiting.
bool check(bool condition, const std::string& what);

// Waits until done() returns true, or the timeout expires.
//
// Sending a message does not block, so a tutorial that wants to observe the
// result has to wait for the actor's thread to get there. Polling for the
// actual condition - rather than sleeping a fixed time - keeps the tutorials
// from being flaky on a loaded machine and slow on a fast one.
bool wait_until(const std::function<bool()>& done, int timeout_ms = 2000);

} // namespace tut

#endif // CAS_TUTORIALS_H
