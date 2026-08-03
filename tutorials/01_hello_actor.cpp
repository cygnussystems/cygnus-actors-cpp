// Tutorial 1: Hello, Actor
//
// TEACHES
//   - defining a message type
//   - defining an actor and registering a handler for that message
//   - the create -> start -> tell -> shutdown lifecycle of the actor system
//
// ASSUMES
//   Nothing. Start here.
//
// THE IDEA
//   An actor is an object that owns some state and processes messages one at
//   a time. You never call an actor's methods directly; you send it a message
//   and the framework delivers it. Because exactly one thread ever runs a
//   given actor's handlers, the actor's own state needs no locking.

#include "tutorials.h"
#include "cas/cas.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <string>
#include <thread>

namespace {

// --- 1. Define a message -----------------------------------------------------
//
// A message is a plain struct deriving from cas::message_base. Put whatever
// data the recipient needs in it. Define messages in your own namespace, not
// in cas::.

struct greet : cas::message_base {
    std::string name;
};

// Observing what an actor did, from outside.
//
// create<T>() hands back an actor_ref - a handle for sending messages - not
// the actor object itself. That is deliberate: it stops callers reaching in
// and touching actor state while the actor's thread is using it.
//
// So to let main() see what happened, the handler records it somewhere both
// threads can reach. An atomic at file scope is the simplest way, and it is
// what the framework's own tests do. Real programs usually reply with another
// message instead (tutorial 3).
std::atomic<int> g_greetings_received{0};

// --- 2. Define an actor ------------------------------------------------------
//
// Derive from cas::actor and override on_start(). The framework calls
// on_start() once, on the actor's own thread, before any message is
// delivered - so it is the right place to register handlers.
//
// Handlers are registered per message type. handler<greet>(...) says "when a
// greet arrives, run this". Registering a type that cannot reach a mailbox
// (one not deriving from message_base) is a compile error at this call, not a
// handler that silently never fires.
//
// Point a handler at a member function. on_start() then reads as a table of
// contents - which messages this actor accepts - with the logic in named
// methods below rather than inline. (A lambda can be registered instead; the
// next tutorial shows when that is worth it.)

class greeter : public cas::actor {
protected:
    void on_start() override {
        // Naming an actor is optional, but it makes log output and registry
        // lookups (a later tutorial) much easier to follow.
        set_name("greeter");

        handler<greet>(&greeter::on_greet);
    }

    // The handler. Takes its message by const reference.
    //
    // This runs on the actor's own thread, and only one thread ever runs it,
    // so any member state of greeter could be touched here without a lock.
    void on_greet(const greet& msg) {
        std::cout << "  greeter says: Hello, " << msg.name << "!\n";
        g_greetings_received.fetch_add(1);
    }
};

} // namespace

namespace tut {

bool run_hello_actor() {
    bool ok = true;

    // --- 3. Create actors BEFORE starting the system -------------------------
    //
    // create<T>() constructs the actor and returns an actor_ref - the handle
    // you send messages through. The actor is not running yet.
    //
    // Order matters: create() before start(). Creating actors after start() is
    // supported, but this order keeps the common case simple.

    section("Creating the actor");
    auto greeter_ref = cas::system::create<greeter>();
    ok &= check(greeter_ref.is_valid(), "create<greeter>() returned a valid ref");

    // --- 4. Start the system -------------------------------------------------
    //
    // start() spins up the worker threads and calls on_start() on every actor
    // created so far. Once it returns, handlers are registered and the actor
    // is ready to receive.

    section("Starting the system");
    cas::system::start();
    ok &= check(cas::system::is_running(), "system reports running");

    // --- 5. Send a message ---------------------------------------------------
    //
    // tell() is fire-and-forget: it copies the message into the actor's
    // mailbox and returns immediately, without waiting for it to be handled.
    // For a call that waits for a reply, see the ask tutorial.

    section("Sending a message");
    greet msg;
    msg.name = "World";
    greeter_ref.tell(msg);

    // Because tell() does not block, the handler has probably not run yet.
    ok &= check(wait_until([] { return g_greetings_received.load() == 1; }),
                "handler ran exactly once");

    // --- 6. Shut down --------------------------------------------------------
    //
    // shutdown() asks every actor to stop and drains messages already queued;
    // wait_for_shutdown() blocks until that has finished. Once it returns, no
    // actor thread is running.
    //
    // Messages sent to a stopped actor are dropped silently - see the dead
    // letter tutorial for how to observe that.

    section("Shutting down");
    cas::system::shutdown();
    cas::system::wait_for_shutdown();
    ok &= check(!cas::system::is_running(), "system reports stopped");

    return ok;
}

} // namespace tut
