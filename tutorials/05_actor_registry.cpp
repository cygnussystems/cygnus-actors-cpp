// Tutorial 5: Finding actors by name
//
// TEACHES
//   - set_name() and what registration gives you
//   - actor_registry::get(), exists()
//   - looking an actor up from inside another actor
//   - what a lookup of an unknown or stopped actor returns
//
// ASSUMES
//   Tutorials 1-4.
//
// THE IDEA
//   Constructor arguments wire actors together up front. The registry is the
//   loose alternative: an actor names itself and anyone can look it up later.
//   The price is that a lookup can fail at runtime, so it must be checked.

#include "tutorials.h"
#include "cas/cas.h"

#include <atomic>
#include <iostream>
#include <string>

namespace {

// The messages.
namespace msg {

struct log_line : cas::message_base {
    std::string text;
};

struct report : cas::message_base {
    std::string text;
};

} // namespace msg

// The actors.
namespace actor {

class logger : public cas::actor {
public:
    // The name others look this actor up by. Names must be unique.
    static constexpr const char* s_registry_name = "logger";

    static inline std::atomic<int> s_line_count{0};  // observed by the tutorial

protected:
    // set_name() in on_start() registers the actor under that name.
    void on_start() override {
        set_name(s_registry_name);
        handler<msg::log_line>(&logger::on_log_line);
    }

private:
    void on_log_line(const msg::log_line& _msg) {
        std::cout << "  [log] " << _msg.text << "\n";
        s_line_count.fetch_add(1);
    }
};

// Holds no reference to the logger: it finds it by name when it needs it.
class reporter : public cas::actor {
public:
    static inline std::atomic<int> s_failed_lookup_count{0};  // observed by the tutorial

protected:
    void on_start() override {
        set_name("reporter");
        handler<msg::report>(&reporter::on_report);
    }

private:
    void on_report(const msg::report& _msg) {
        // get() returns an invalid ref if nothing is registered under the name
        // or the actor has stopped. Telling an invalid ref goes nowhere.
        auto logger = cas::actor_registry::get(logger::s_registry_name);
        if (!logger.is_valid()) {
            s_failed_lookup_count.fetch_add(1);
            return;
        }
        msg::log_line line;
        line.text = _msg.text;
        logger.tell(line);
    }
};

} // namespace actor

} // namespace

namespace tut {

bool run_actor_registry() {
    bool ok = true;

    heading("Starting two named actors");
    //-----------------------------------
    cas::system::create<actor::logger>();  // no need to keep the ref: it can be found by name
    auto reporter = cas::system::create<actor::reporter>();
    cas::system::start();

    heading("Looking actors up by name");
    //-----------------------------------
    // Names are registered in on_start(), so they are visible once start() returns.
    auto found = cas::actor_registry::get("logger");
    ok &= check(cas::actor_registry::exists("logger"), "exists(\"logger\") is true");
    ok &= check(found.is_valid() && found.name() == "logger",
                "get(\"logger\") returned a valid ref with that name");

    heading("Looking up a name that was never registered");
    //-----------------------------------------------------
    // Not an error, no exception - just an invalid ref.
    ok &= check(!cas::actor_registry::exists("nonexistent"),
                "exists() is false for an unregistered name");
    ok &= check(!cas::actor_registry::get("nonexistent").is_valid(),
                "get() returns an invalid ref rather than throwing");

    heading("One actor finding another by name");
    //-------------------------------------------
    msg::report req;
    req.text = "hello from the reporter";
    reporter.tell(req);
    ok &= check(wait_until(actor::logger::s_line_count, 1),
                "reporter found the logger and the message arrived");
    ok &= check(actor::reporter::s_failed_lookup_count.load() == 0, "no lookup failed");

    heading("Sending straight through a looked-up ref");
    //--------------------------------------------------
    msg::log_line direct;
    direct.text = "sent from main()";
    cas::actor_registry::get("logger").tell(direct);
    ok &= check(wait_until(actor::logger::s_line_count, 2),
                "message sent via registry lookup arrived");

    heading("Shutting down");
    //-----------------------
    cas::system::shutdown();
    cas::system::wait_for_shutdown();
    return ok;
}

} // namespace tut
