// Tutorial 6: The call pattern (request/response with a return value)
//
// TEACHES
//   - call_handler() and operation tags
//   - call<R>() to call an actor and get a value back
//   - the timeout overload, which returns std::optional
//   - how exceptions in a call handler surface at the caller
//   - the cases where call() would deadlock and is rejected instead
//
// ASSUMES
//   Tutorials 1-5.
//
// THE IDEA
//   tell() is one-way. call() blocks the caller until the actor returns a
//   value - convenient, and occasionally dangerous. Most of this tutorial is
//   about the danger.

#include "tutorials.h"
#include "cas/cas.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {

class calculator : public cas::actor {
public:
    // Operation tags: empty types that name an operation. A call is dispatched
    // on its tag, so one actor can expose several operations, each with its
    // own signature.
    struct add_op {};
    struct divide_op {};
    struct slow_op {};
    struct self_ask_op {};

protected:
    void on_start() override {
        set_name("calculator");
        // call_handler<ReturnType, Tag>(&method): an ordinary member function
        // with ordinary arguments - call()'s extra arguments are forwarded to it.
        call_handler<int, add_op>(&calculator::add);
        call_handler<double, divide_op>(&calculator::divide);
        call_handler<int, slow_op>(&calculator::slow);
        call_handler<int, self_ask_op>(&calculator::ask_myself);
    }

private:
    int add(int a, int b) { return a + b; }

    // Throwing reports failure: the exception is rethrown in the caller.
    double divide(int a, int b) {
        if (b == 0) {
            throw std::runtime_error("division by zero");
        }
        return static_cast<double>(a) / b;
    }

    // Blocks this actor's thread - it handles nothing else meanwhile. Call
    // handlers should be quick, like any handler.
    int slow(int ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        return ms;
    }

    // Deliberately wrong: the thread that would answer is the one waiting.
    // The framework throws call_deadlock_error instead of hanging.
    int ask_myself() { return self().call<int>(add_op{}, 1, 1); }
};

} // namespace

namespace tut {

bool run_call() {
    using namespace std::chrono_literals;
    bool ok = true;

    heading("Starting");
    //------------------
    auto calc = cas::system::create<calculator>();
    cas::system::start();

    heading("Asking for a value");
    //----------------------------
    // call<Return>(tag, args...) blocks until the handler has returned.
    const int sum = calc.call<int>(calculator::add_op{}, 10, 5);
    const double quotient = calc.call<double>(calculator::divide_op{}, 7, 2);
    std::cout << "  add(10, 5) = " << sum << ", divide(7, 2) = " << quotient << "\n";
    ok &= check(sum == 15, "call<int>(add_op{}, 10, 5) returned 15");
    ok &= check(quotient == 3.5, "call<double>(divide_op{}, 7, 2) returned 3.5");

    heading("Asking with a timeout");
    //-------------------------------
    // Returns an empty optional instead of blocking forever. Prefer it wherever
    // the target might be slow or stuck.
    auto quick = calc.call<int>(calculator::add_op{}, 1000ms, 2, 3);
    ok &= check(quick.has_value() && *quick == 5,
                "a fast operation returned a value within the timeout");
    auto late = calc.call<int>(calculator::slow_op{}, 50ms, 500);
    ok &= check(!late.has_value(), "a slow operation timed out and returned an empty optional");

    heading("A call handler that throws");
    //------------------------------------
    bool caught = false;
    try {
        (void)calc.call<double>(calculator::divide_op{}, 1, 0);
    } catch (const std::exception& e) {
        caught = true;
        std::cout << "  caught: " << e.what() << "\n";
    }
    ok &= check(caught, "exception from the handler surfaced at the caller");

    heading("Deadlock protection");
    //-----------------------------
    // call() deadlocks if the caller holds the thread the target needs. The
    // framework rejects these cases up front instead of hanging:
    //   1. call() from on_start()
    //   2. call() to an actor sharing the caller's worker thread
    //   3. an actor calling itself (shown here)
    // Rule: from inside a handler, prefer tell() and a reply message
    // (tutorial 3). main() is not an actor, so its calls above were safe.
    bool rejected = false;
    try {
        (void)calc.call<int>(calculator::self_ask_op{}, 2000ms);
    } catch (const std::exception& e) {
        rejected = true;
        std::cout << "  rejected: " << e.what() << "\n";
    }
    ok &= check(rejected, "an actor asking itself was rejected, not left to deadlock");

    heading("Shutting down");
    //-----------------------
    cas::system::shutdown();
    cas::system::wait_for_shutdown();
    return ok;
}

} // namespace tut
