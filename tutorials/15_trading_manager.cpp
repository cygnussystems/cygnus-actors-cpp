// Tutorial 15: A manager that launches and retires actors at runtime
//
// TEACHES
//   - creating actors from inside a handler while the system runs
//   - one pooled actor per subscriber, grouped under the actor they serve
//   - an actor that finishes by stopping itself (stop_actor(self()))
//   - watch() + termination_msg to tear down everything that belonged to it
//   - stopping actors from a handler without blocking (wait_for_stop = false)
//   - a failing handler: on_handler_exception() -> stop, handled the same way
//
// ASSUMES
//   Tutorials 1-9 (especially 8, stopping actors, and 9, watching).
//
// THE IDEA
//   A trading server launches trading systems while it runs. Each trading
//   system is an actor, and each client streaming its data gets a small actor
//   of its own. When a trading system completes - or fails - its streams must
//   go too. One manager actor owns that bookkeeping: it creates the actors,
//   watches the trading system, and when told the trading system has
//   terminated, stops the streams that belonged to it.
//
//   The same shape fits any "session with helpers": a connection and its
//   subscriptions, a job and its workers, an order and its child orders.

#include "tutorials.h"
#include "cas/cas.h"

#include <atomic>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// --- Messages ------------------------------------------------------------------

// To the manager: start a trading system and one stream per subscriber
struct launch_trading_system : cas::message_base {
    std::string system_id;
    std::vector<std::string> subscribers;
};

// To the manager: a client stopped listening
struct unsubscribe : cas::message_base {
    std::string system_id;
    std::string subscriber;
};

// To a trading system: market data, and "your run is over"
struct market_update : cas::message_base {
    double price = 0.0;
};
struct run_complete : cas::message_base {};

// To a trading system, from the manager: stop streaming to this actor
struct drop_stream : cas::message_base {
    cas::actor_ref stream;
};

// --- What main() observes ------------------------------------------------------

std::mutex g_mutex;
std::map<std::string, int> g_updates_by_subscriber;  // guarded by g_mutex
std::atomic<int> g_streams_stopped{0};
std::atomic<int> g_systems_retired{0};
std::atomic<int> g_failures_reported{0};

int updates_for(const std::string& subscriber) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_updates_by_subscriber.find(subscriber);
    return it == g_updates_by_subscriber.end() ? 0 : it->second;
}

// --- One streaming actor per subscriber -------------------------------------------
//
// Deliberately a plain pooled actor. There may be hundreds of these; a
// fast_actor each would mean a dedicated (polling) thread each. Unnamed: the
// manager holds its actor_ref, so nobody needs to look it up.

class stream_actor : public cas::actor {
public:
    explicit stream_actor(std::string subscriber) : m_subscriber(std::move(subscriber)) {}

protected:
    void on_start() override {
        handler<market_update>(&stream_actor::on_update);
    }

    void on_update(const market_update&) {
        // A real stream would write to the client's connection here
        std::lock_guard<std::mutex> lock(g_mutex);
        ++g_updates_by_subscriber[m_subscriber];
    }

    void on_stop() override {
        g_streams_stopped.fetch_add(1);
    }

private:
    std::string m_subscriber;
};

// --- The trading system ---------------------------------------------------------

class trading_system_actor : public cas::actor {
public:
    trading_system_actor(std::string id, std::vector<cas::actor_ref> streams)
        : m_id(std::move(id)), m_streams(std::move(streams)) {}

protected:
    void on_start() override {
        // Named, so the outside world (here: main) can reach it by id
        set_name("ts." + m_id);
        handler<market_update>(&trading_system_actor::on_update);
        handler<drop_stream>(&trading_system_actor::on_drop_stream);
        handler<run_complete>(&trading_system_actor::on_complete);
    }

    void on_update(const market_update& msg) {
        if (msg.price < 0.0) {
            // A bug, or data it cannot handle - see on_handler_exception()
            throw std::runtime_error("negative price");
        }
        for (const auto& stream : m_streams) {
            stream.tell(msg);
        }
    }

    void on_drop_stream(const drop_stream& msg) {
        std::erase(m_streams, msg.stream);
    }

    // The run is over: the trading system stops itself. From its own handler
    // this returns at once; the stop completes after the handler returns, and
    // the manager (watching) is told.
    void on_complete(const run_complete&) {
        std::cout << "  ts." << m_id << ": run complete, stopping\n";
        cas::system::stop_actor(self());
    }

    // A handler threw. Its state may be half-updated, so don't carry on:
    // stop. To the manager this looks exactly like a completed run.
    cas::exception_action on_handler_exception(const cas::message_base&,
                                               std::exception_ptr) override {
        std::cout << "  ts." << m_id << ": handler failed, stopping\n";
        return cas::exception_action::stop;
    }

    void on_stop() override {
        m_streams.clear();  // drop our references (see "Memory" in doc/70)
    }

private:
    std::string m_id;
    std::vector<cas::actor_ref> m_streams;
};

// --- The manager ----------------------------------------------------------------

class trading_manager : public cas::actor {
protected:
    void on_start() override {
        set_name("trading_manager");
        handler<launch_trading_system>(&trading_manager::on_launch);
        handler<unsubscribe>(&trading_manager::on_unsubscribe);
        handler<cas::termination_msg>(&trading_manager::on_terminated);
    }

    void on_launch(const launch_trading_system& msg) {
        // create() from a handler: each new actor's on_start() has run by the
        // time create() returns, so it is ready for messages at once.
        std::map<std::string, cas::actor_ref> streams;
        std::vector<cas::actor_ref> stream_refs;
        for (const auto& subscriber : msg.subscribers) {
            auto stream = cas::system::create<stream_actor>(subscriber);
            streams[subscriber] = stream;
            stream_refs.push_back(stream);
        }

        auto ts = cas::system::create<trading_system_actor>(msg.system_id, stream_refs);

        // Be told when it terminates - however that happens
        cas::system::watch(self(), ts);

        // Keyed by instance id, which termination_msg carries. Names can be
        // reused by a relaunch; instance ids are never reused.
        m_systems[ts.get_actor()->instance_id()] = running_system{msg.system_id, ts, std::move(streams)};
        std::cout << "  manager: launched " << msg.system_id << " with "
                  << msg.subscribers.size() << " stream(s)\n";
    }

    void on_unsubscribe(const unsubscribe& msg) {
        for (auto& entry : m_systems) {
            running_system& sys = entry.second;
            if (sys.system_id != msg.system_id) continue;
            auto it = sys.streams.find(msg.subscriber);
            if (it == sys.streams.end()) return;

            // Tell the trading system to stop sending to it, then stop it.
            // Updates the trading system sends in between are dead letters -
            // expected, harmless, and counted.
            drop_stream drop;
            drop.stream = it->second;
            sys.trading_system.tell(drop);
            stop_without_blocking(it->second);
            sys.streams.erase(it);
            std::cout << "  manager: " << msg.subscriber << " unsubscribed from "
                      << msg.system_id << "\n";
            return;
        }
    }

    // The trading system stopped - completed, failed, or stopped by someone
    // else. Either way its streams go with it.
    void on_terminated(const cas::termination_msg& msg) {
        auto it = m_systems.find(msg.instance_id);
        if (it == m_systems.end()) return;

        for (const auto& entry : it->second.streams) {
            stop_without_blocking(entry.second);
        }
        std::cout << "  manager: retired " << it->second.system_id << " and its "
                  << it->second.streams.size() << " stream(s)\n";
        m_systems.erase(it);  // drop our references so the actors can be freed
        g_systems_retired.fetch_add(1);
    }

private:
    // From a handler, never wait for a stop: that would block this worker
    // thread (and every actor on it) until the other actor has finished.
    static void stop_without_blocking(const cas::actor_ref& ref) {
        cas::stop_config config;
        config.wait_for_stop = false;
        cas::system::stop_actor(ref, config);
    }

    struct running_system {
        std::string system_id;
        cas::actor_ref trading_system;
        std::map<std::string, cas::actor_ref> streams;  // subscriber -> stream
    };
    std::unordered_map<size_t, running_system> m_systems;
};

// Sends one update to a running trading system, looked up by name
void publish(const std::string& system_id, double price) {
    market_update update;
    update.price = price;
    cas::actor_registry::get("ts." + system_id).tell(update);
}

} // namespace

namespace tut {

bool run_trading_manager() {
    bool ok = true;

    // Handler exceptions are reported here (without a callback they would go
    // to stderr). The trading system's failure in part 4 lands in it.
    cas::system::set_handler_exception_handler([](const cas::handler_exception_info& info) {
        std::cout << "  [report] " << info.actor_name << ": " << info.what << "\n";
        g_failures_reported.fetch_add(1);
    });

    section("Starting the system with just the manager");
    auto manager = cas::system::create<trading_manager>();
    cas::system::start();
    const size_t baseline = cas::system::actor_count();  // the manager

    // --- 1. Launching trading systems at runtime -------------------------------

    section("Launching two trading systems");
    launch_trading_system es;
    es.system_id = "es";
    es.subscribers = {"alice", "bob"};
    manager.tell(es);

    launch_trading_system nq;
    nq.system_id = "nq";
    nq.subscribers = {"carol", "dave"};
    manager.tell(nq);

    ok &= check(wait_until([] { return cas::actor_registry::exists("ts.es") &&
                                       cas::actor_registry::exists("ts.nq"); }),
                "both trading systems are running");
    ok &= check(cas::system::actor_count() == baseline + 6,
                "two trading systems and four streams were created");

    publish("es", 101.5);
    publish("nq", 202.25);
    ok &= check(wait_until([] { return updates_for("alice") == 1 && updates_for("bob") == 1 &&
                                       updates_for("carol") == 1 && updates_for("dave") == 1; }),
                "each subscriber's stream received its system's update");

    // --- 2. A subscriber leaves while its trading system keeps running ------------

    section("carol unsubscribes from nq");
    unsubscribe leave;
    leave.system_id = "nq";
    leave.subscriber = "carol";
    manager.tell(leave);

    ok &= check(wait_until([] { return g_streams_stopped.load() == 1; }),
                "carol's stream was stopped");
    ok &= check(cas::actor_registry::get("ts.nq").is_running(),
                "nq keeps running");

    // --- 3. A trading system completes -------------------------------------------

    section("es completes its run");
    cas::actor_registry::get("ts.es").tell(run_complete{});

    ok &= check(wait_until([] { return g_systems_retired.load() == 1; }),
                "the manager was told es terminated");
    ok &= check(wait_until([] { return g_streams_stopped.load() == 3; }),
                "alice's and bob's streams were stopped with it");
    ok &= check(!cas::actor_registry::exists("ts.es"),
                "es's name was released");

    // --- 4. A trading system fails ---------------------------------------------------

    section("nq's handler throws");
    publish("nq", -1.0);  // on_update throws; on_handler_exception() says stop

    ok &= check(wait_until([] { return g_failures_reported.load() == 1; }),
                "the exception was reported, not fatal");
    ok &= check(wait_until([] { return g_systems_retired.load() == 2; }),
                "the manager retired nq like a completed run");
    ok &= check(wait_until([] { return g_streams_stopped.load() == 4; }),
                "dave's stream was stopped with it");

    // Everything the manager created is gone; only the manager is left
    ok &= check(wait_until([&] { return cas::system::actor_count() == baseline; }),
                "only the manager remains");
    ok &= check(manager.is_running(), "the manager is still running");

    section("Shutting down");
    cas::system::shutdown();
    cas::system::wait_for_shutdown();
    cas::system::clear_handler_exception_handler();

    return ok;
}

} // namespace tut
