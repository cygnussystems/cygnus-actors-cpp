// Tutorial 5: Finding actors by name
//
// TEACHES
//   - set_name() and what registration gives you
//   - actor_registry::get(), exists() and count()
//   - looking an actor up from inside another actor
//   - what a lookup of an unknown or stopped actor returns
//
// ASSUMES
//   Tutorials 1-4.
//
// THE IDEA
//   So far every actor got its collaborators by constructor argument, which
//   means wiring everything together up front. The registry is the loose
//   alternative: an actor names itself, and anyone can look it up later. The
//   trade is that a lookup can fail at runtime, so it has to be checked.

#include "tutorials.h"
#include "cas/cas.h"

#include <atomic>
#include <iostream>
#include <string>

namespace {

struct log_line : cas::message_base {
    std::string text;
};

// Tells an actor to find the logger itself, rather than being handed it.
struct log_via_lookup : cas::message_base {
    std::string text;
};

std::atomic<int> g_lines_logged{0};
std::atomic<int> g_failed_lookups{0};

// --- 1. An actor that names itself -------------------------------------------
//
// set_name() in on_start() registers the actor under that name. The name must
// be unique; it is how everything else finds this actor.

class logger : public cas::actor {
protected:
    void on_start() override {
        set_name("logger");
        handler<log_line>(&logger::on_log_line);
    }

    void on_log_line(const log_line& msg) {
        std::cout << "  [log] " << msg.text << "\n";
        g_lines_logged.fetch_add(1);
    }
};

// --- 2. An actor that looks another up ---------------------------------------
//
// This actor holds no reference to the logger. It finds it by name at the
// moment it needs it.

class reporter : public cas::actor {
protected:
    void on_start() override {
        set_name("reporter");
        handler<log_via_lookup>(&reporter::on_log_via_lookup);
    }

    void on_log_via_lookup(const log_via_lookup& msg) {
        // get() returns an actor_ref that may be invalid - if no actor
        // registered under that name, or if it has since stopped. Always
        // check: telling through an invalid ref silently goes nowhere.
        auto logger_ref = cas::actor_registry::get("logger");

        if (!logger_ref.is_valid()) {
            std::cout << "  reporter: no logger registered\n";
            g_failed_lookups.fetch_add(1);
            return;
        }

        log_line line;
        line.text = msg.text;
        logger_ref.tell(line);
    }
};

} // namespace

namespace tut {

bool run_actor_registry() {
    bool ok = true;

    section("Starting two named actors");
    cas::system::create<logger>();
    auto reporter_ref = cas::system::create<reporter>();
    cas::system::start();

    // --- 3. Looking up from outside an actor ---------------------------------
    //
    // The registry is reachable from anywhere, including main(). Names are
    // registered during on_start(), so a lookup right after start() succeeds.

    section("Looking actors up by name");

    ok &= check(cas::actor_registry::exists("logger"),
                "exists(\"logger\") is true");

    auto found = cas::actor_registry::get("logger");
    ok &= check(found.is_valid(), "get(\"logger\") returned a valid ref");
    ok &= check(found.name() == "logger", "the ref reports the name we asked for");

    // --- 4. A lookup that fails ----------------------------------------------
    //
    // An unknown name is not an error and does not throw. You get an invalid
    // ref, which is why every lookup needs checking.

    section("Looking up a name that was never registered");
    ok &= check(!cas::actor_registry::exists("nonexistent"),
                "exists() is false for an unregistered name");
    ok &= check(!cas::actor_registry::get("nonexistent").is_valid(),
                "get() returns an invalid ref rather than throwing");

    // --- 5. Lookup from inside an actor --------------------------------------

    section("One actor finding another by name");
    log_via_lookup req;
    req.text = "hello from the reporter";
    reporter_ref.tell(req);

    ok &= check(wait_until([] { return g_lines_logged.load() == 1; }),
                "reporter found the logger and the message arrived");
    ok &= check(g_failed_lookups.load() == 0, "no lookup failed");

    // --- 6. Sending directly to a name ---------------------------------------

    section("Sending straight through a looked-up ref");
    log_line direct;
    direct.text = "sent from main()";
    cas::actor_registry::get("logger").tell(direct);

    ok &= check(wait_until([] { return g_lines_logged.load() == 2; }),
                "message sent via registry lookup arrived");

    section("Shutting down");
    cas::system::shutdown();
    cas::system::wait_for_shutdown();

    return ok;
}

} // namespace tut
