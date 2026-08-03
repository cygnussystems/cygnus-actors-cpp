// Tutorial 6: The ask pattern (request/response with a return value)
//
// TEACHES
//   - ask_handler() and operation tags
//   - ask<R>() to call an actor and get a value back
//   - the timeout overload, which returns std::optional
//   - how exceptions in an ask handler surface at the caller
//   - the three cases where ask() would deadlock and is rejected instead
//
// ASSUMES
//   Tutorials 1-5.
//
// THE IDEA
//   tell() is one-way. Sometimes you genuinely want a value back - a lookup,
//   a computation, a health check. ask() blocks the caller until the actor
//   replies, which makes it convenient and occasionally dangerous. The danger
//   is worth understanding, so most of this tutorial is about it.

#include "tutorials.h"
#include "cas/cas.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

// --- 1. Operation tags -------------------------------------------------------
//
// An ask is dispatched on a *tag* type, not a message type. The tag is an
// empty struct whose only job is to name the operation, which is what lets one
// actor expose several ask operations with different signatures.

struct add_op {};
struct divide_op {};
struct slow_op {};
struct self_ask_op {};

class calculator : public cas::actor {
protected:
    void on_start() override {
        set_name("calculator");

        // ask_handler<ReturnType, OpTag>(&method).
        //
        // The method is an ordinary member function with ordinary arguments -
        // no message struct needed. Arguments passed to ask() are forwarded to
        // it directly.
        ask_handler<int, add_op>(&calculator::add);
        ask_handler<double, divide_op>(&calculator::divide);
        ask_handler<int, slow_op>(&calculator::slow);
        ask_handler<int, self_ask_op>(&calculator::ask_myself);
    }

    int add(int a, int b) {
        return a + b;
    }

    double divide(int a, int b) {
        // Throwing is a legitimate way to report failure. The exception is
        // carried back and rethrown in the caller - see section 4.
        if (b == 0) {
            throw std::runtime_error("division by zero");
        }
        return static_cast<double>(a) / b;
    }

    // Deliberately does the wrong thing, so section 5 can show the framework
    // catching it. This runs on the actor's own thread, so the ask() below is
    // this actor asking itself - it could never reply, because the thread
    // that would run the reply is the one blocked waiting for it.
    int ask_myself() {
        return self().ask<int>(add_op{}, 1, 1);  // throws ask_deadlock_error
    }

    int slow(int ms) {
        // Deliberately slower than the timeout used below.
        //
        // Note this blocks the actor's thread: while it sleeps, this actor
        // handles nothing else. An ask handler should be quick for the same
        // reason any handler should be.
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        return ms;
    }
};

} // namespace

namespace tut {

bool run_ask_pattern() {
    bool ok = true;

    section("Starting");
    auto calc = cas::system::create<calculator>();
    cas::system::start();

    // --- 2. A simple ask -----------------------------------------------------
    //
    // ask<int>(add_op{}, 10, 5) blocks until the actor runs add(10, 5) and
    // returns. The return type is given explicitly; the tag selects the
    // operation; the rest are the method's arguments.

    section("Asking for a value");
    const int sum = calc.ask<int>(add_op{}, 10, 5);
    std::cout << "  add(10, 5) = " << sum << "\n";
    ok &= check(sum == 15, "ask<int>(add_op{}, 10, 5) returned 15");

    const double quotient = calc.ask<double>(divide_op{}, 7, 2);
    std::cout << "  divide(7, 2) = " << quotient << "\n";
    ok &= check(quotient == 3.5, "ask<double>(divide_op{}, 7, 2) returned 3.5");

    // --- 3. Asking with a timeout --------------------------------------------
    //
    // The timeout overload returns std::optional and gives up rather than
    // blocking forever. Prefer it anywhere the target might be slow or wedged:
    // a plain ask() against a stuck actor blocks the caller indefinitely.

    section("Asking with a timeout");

    auto quick = calc.ask<int>(add_op{}, std::chrono::milliseconds(1000), 2, 3);
    ok &= check(quick.has_value() && *quick == 5,
                "a fast operation returned a value within the timeout");

    auto slow = calc.ask<int>(slow_op{}, std::chrono::milliseconds(50), 500);
    ok &= check(!slow.has_value(),
                "a slow operation timed out and returned an empty optional");
    std::cout << "  slow_op timed out as expected\n";

    // --- 4. Exceptions -------------------------------------------------------
    //
    // An exception thrown inside the ask handler is transported back and
    // rethrown here, so ordinary try/catch works across the actor boundary.

    section("An ask handler that throws");
    bool caught = false;
    try {
        (void)calc.ask<double>(divide_op{}, 1, 0);
    } catch (const std::exception& e) {
        caught = true;
        std::cout << "  caught: " << e.what() << "\n";
    }
    ok &= check(caught, "exception from the handler surfaced at the caller");

    // --- 5. When ask() would deadlock ----------------------------------------
    //
    // ask() blocks the calling thread until the target replies. If the target
    // can never reply because the caller is holding the very thread it needs,
    // that is a deadlock. The framework detects three such cases up front and
    // throws instead of hanging:
    //
    //   1. ask() from on_start()   - no worker is guaranteed to be running yet
    //   2. ask() to an actor sharing the caller's worker thread
    //   3. an actor asking itself
    //
    // The rule to remember: never ask() from inside a handler unless you know
    // the target is on a different thread. Use tell() and handle the reply as
    // a message (tutorial 3) - that is always safe.
    //
    // Every ask() above came from main(), which is not an actor and so shares
    // no worker thread with anyone. Below, an actor asks *itself* to show the
    // check firing rather than just describing it.

    section("Deadlock protection");

    bool rejected = false;
    try {
        // self_ask_op is handled by an actor that asks itself from inside a
        // handler. The framework rejects it instead of hanging.
        calc.ask<int>(self_ask_op{}, std::chrono::milliseconds(2000));
    } catch (const std::exception& e) {
        rejected = true;
        std::cout << "  rejected: " << e.what() << "\n";
    }
    ok &= check(rejected,
                "an actor asking itself was rejected, not left to deadlock");

    section("Shutting down");
    cas::system::shutdown();
    cas::system::wait_for_shutdown();

    return ok;
}

} // namespace tut
