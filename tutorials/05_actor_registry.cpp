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

class logger : public cas::actor {
public:
    struct log_line : cas::message_base {
        std::string text;
    };

    // The name others look this actor up by. Names must be unique.
    static constexpr const char* registry_name = "logger";

    static inline std::atomic<int> lines{0};  // observed by the tutorial

protected:
    // set_name() in on_start() registers the actor under that name.
    void on_start() override {
        set_name(registry_name);
        handler<log_line>(&logger::on_log_line);
    }

private:
    void on_log_line(const log_line& msg) {
        std::cout << "  [log] " << msg.text << "\n";
        lines.fetch_add(1);
    }
};

// Holds no reference to the logger: it finds it by name when it needs it.
class reporter : public cas::actor {
public:
    struct report : cas::message_base {
        std::string text;
    };

    static inline std::atomic<int> failed_lookups{0};  // observed by the tutorial

protected:
    void on_start() override {
        set_name("reporter");
        handler<report>(&reporter::on_report);
    }

private:
    void on_report(const report& msg) {
        // get() returns an invalid ref if nothing is registered under the name
        // or the actor has stopped. Telling an invalid ref goes nowhere.
        auto logger_ref = cas::actor_registry::get(logger::registry_name);
        if (!logger_ref.is_valid()) {
            failed_lookups.fetch_add(1);
            return;
        }
        logger::log_line line;
        line.text = msg.text;
        logger_ref.tell(line);
    }
};

} // namespace

namespace tut {

bool run_actor_registry() {
    bool ok = true;

    section("Starting two named actors");
    cas::system::create<logger>();  // no need to keep the ref: it can be found by name
    auto reporter_ref = cas::system::create<reporter>();
    cas::system::start();

    section("Looking actors up by name");
    // Names are registered in on_start(), so they are visible once start() returns.
    auto found = cas::actor_registry::get("logger");
    ok &= check(cas::actor_registry::exists("logger"), "exists(\"logger\") is true");
    ok &= check(found.is_valid() && found.name() == "logger",
                "get(\"logger\") returned a valid ref with that name");

    section("Looking up a name that was never registered");
    // Not an error, no exception - just an invalid ref.
    ok &= check(!cas::actor_registry::exists("nonexistent"),
                "exists() is false for an unregistered name");
    ok &= check(!cas::actor_registry::get("nonexistent").is_valid(),
                "get() returns an invalid ref rather than throwing");

    section("One actor finding another by name");
    reporter::report req;
    req.text = "hello from the reporter";
    reporter_ref.tell(req);
    ok &= check(wait_until([] { return logger::lines.load() == 1; }),
                "reporter found the logger and the message arrived");
    ok &= check(reporter::failed_lookups.load() == 0, "no lookup failed");

    section("Sending straight through a looked-up ref");
    logger::log_line direct;
    direct.text = "sent from main()";
    cas::actor_registry::get("logger").tell(direct);
    ok &= check(wait_until([] { return logger::lines.load() == 2; }),
                "message sent via registry lookup arrived");

    section("Shutting down");
    cas::system::shutdown();
    cas::system::wait_for_shutdown();
    return ok;
}

} // namespace tut
