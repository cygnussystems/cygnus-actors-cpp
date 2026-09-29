// Tutorial 1: Hello, Actor
//
// TEACHES
//   - defining an actor and the message it handles
//   - the create -> start -> tell -> shutdown lifecycle
//
// THE IDEA
//   An actor is a self-contained object: it owns its state, and the only way
//   to affect it is to send it a message. Exactly one thread runs an actor's
//   handlers at a time, so its state needs no locks.

#include "tutorials.h"
#include "cas/cas.h"

#include <atomic>
#include <iostream>
#include <string>

namespace {

// An actor, with the message it understands declared inside it: the actor's
// protocol is part of the actor.
class greeter : public cas::actor {
public:
    // Messages derive from cas::message_base. They are not aggregates (the
    // base has a virtual destructor): construct, set the fields, send.
    struct greet : cas::message_base {
        std::string name;
    };

    // Observed by the tutorial. main() cannot reach into the actor - create()
    // hands out an actor_ref, not the object - so the actor publishes what it
    // did through a static atomic. Real programs reply with a message instead
    // (tutorial 3).
    static inline std::atomic<int> greetings{0};

protected:
    // Called once, on the actor's thread, before any message arrives:
    // register the handlers here.
    void on_start() override {
        set_name("greeter");
        handler<greet>(&greeter::on_greet);
    }

private:
    void on_greet(const greet& msg) {
        std::cout << "  greeter: Hello, " << msg.name << "!\n";
        greetings.fetch_add(1);
    }
};

} // namespace

namespace tut {

bool run_hello_actor() {
    bool ok = true;

    heading("Create, then start");
    // create() builds the actor and returns a handle; start() runs every
    // actor's on_start() and the worker threads.
    auto greeter_ref = cas::system::create<greeter>();
    cas::system::start();
    ok &= check(greeter_ref.is_valid() && cas::system::is_running(), "actor created, system running");

    heading("Send a message");
    // tell() copies the message into the actor's mailbox and returns at once;
    // the handler runs later, on the actor's thread.
    greeter::greet msg;
    msg.name = "World";
    greeter_ref.tell(msg);
    ok &= check(wait_until([] { return greeter::greetings.load() == 1; }), "the handler ran once");

    heading("Shut down");
    // shutdown() lets queued messages drain; wait_for_shutdown() waits for it.
    cas::system::shutdown();
    cas::system::wait_for_shutdown();
    ok &= check(!cas::system::is_running(), "system stopped");

    return ok;
}

} // namespace tut
