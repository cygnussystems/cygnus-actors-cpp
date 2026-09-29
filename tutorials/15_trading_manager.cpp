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
//   A trading server launches trading systems while it runs; each client
//   streaming a system's data gets a small actor of its own. When a trading
//   system completes - or fails - its streams must go too. One manager owns
//   that bookkeeping: it creates the actors, watches each trading system, and
//   when told one has terminated, stops the streams that belonged to it.
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

// Protocol: market data is handled by both trading systems and streams.
struct market_update : cas::message_base {
    double price = 0.0;
};

// One per subscriber. Deliberately a plain pooled actor: there may be hundreds,
// and a fast_actor each would mean a dedicated thread each. Unnamed - the
// manager holds its actor_ref, so nobody needs to look it up.
class stream : public cas::actor {
public:
    explicit stream(std::string subscriber) : m_subscriber(std::move(subscriber)) {}

    static inline std::atomic<int> stopped{0};  // observed by the tutorial

    static int updates_for(const std::string& subscriber) {
        std::lock_guard<std::mutex> lock(s_mutex);
        auto it = s_updates.find(subscriber);
        return it == s_updates.end() ? 0 : it->second;
    }

protected:
    void on_start() override { handler<market_update>(&stream::on_update); }
    void on_stop() override { stopped.fetch_add(1); }

private:
    void on_update(const market_update&) {
        // A real stream would write to the client's connection here
        std::lock_guard<std::mutex> lock(s_mutex);
        ++s_updates[m_subscriber];
    }

    std::string m_subscriber;

    static inline std::mutex s_mutex;
    static inline std::map<std::string, int> s_updates;  // subscriber -> count
};

class trading_system : public cas::actor {
public:
    struct run_complete : cas::message_base {};  // "your run is over"
    struct drop_stream : cas::message_base {     // from the manager
        cas::actor_ref stream;
    };

    trading_system(std::string id, std::vector<cas::actor_ref> streams)
        : m_id(std::move(id)), m_streams(std::move(streams)) {}

    // Named, so the outside world (here: main) can reach it by id
    static std::string name_for(const std::string& id) { return "ts." + id; }
    static cas::actor_ref find(const std::string& id) { return cas::actor_registry::get(name_for(id)); }

protected:
    void on_start() override {
        set_name(name_for(m_id));
        handler<market_update>(&trading_system::on_update);
        handler<drop_stream>(&trading_system::on_drop_stream);
        handler<run_complete>(&trading_system::on_complete);
    }

    // A handler threw. Its state may be half-updated, so don't carry on:
    // stop. To the manager this looks exactly like a completed run.
    cas::exception_action on_handler_exception(const cas::message_base&, std::exception_ptr) override {
        std::cout << "  ts." << m_id << ": handler failed, stopping\n";
        return cas::exception_action::stop;
    }

    void on_stop() override { m_streams.clear(); }  // drop our references (see "Memory" in doc/70)

private:
    void on_update(const market_update& msg) {
        if (msg.price < 0.0) {
            throw std::runtime_error("negative price");  // a bug, or data it cannot handle
        }
        for (const auto& s : m_streams) {
            s.tell(msg);
        }
    }

    void on_drop_stream(const drop_stream& msg) { std::erase(m_streams, msg.stream); }

    // Stopping itself from its own handler returns at once; the stop completes
    // after the handler returns, and the manager (watching) is told.
    void on_complete(const run_complete&) {
        std::cout << "  ts." << m_id << ": run complete, stopping\n";
        cas::system::stop_actor(self());
    }

    std::string m_id;
    std::vector<cas::actor_ref> m_streams;
};

class trading_manager : public cas::actor {
public:
    struct launch : cas::message_base {  // a trading system + one stream per subscriber
        std::string system_id;
        std::vector<std::string> subscribers;
    };
    struct unsubscribe : cas::message_base {  // a client stopped listening
        std::string system_id;
        std::string subscriber;
    };

    static inline std::atomic<int> retired{0};  // observed by the tutorial

protected:
    void on_start() override {
        set_name("trading_manager");
        handler<launch>(&trading_manager::on_launch);
        handler<unsubscribe>(&trading_manager::on_unsubscribe);
        handler<cas::termination_msg>(&trading_manager::on_terminated);
    }

private:
    struct running_system {
        std::string system_id;
        cas::actor_ref ts;
        std::map<std::string, cas::actor_ref> streams;  // subscriber -> stream
    };

    void on_launch(const launch& msg) {
        // create() from a handler: each new actor's on_start() has run by the
        // time create() returns, so it is ready for messages at once.
        running_system sys{msg.system_id, {}, {}};
        std::vector<cas::actor_ref> refs;
        for (const auto& subscriber : msg.subscribers) {
            auto s = cas::system::create<stream>(subscriber);
            sys.streams[subscriber] = s;
            refs.push_back(s);
        }
        sys.ts = cas::system::create<trading_system>(msg.system_id, refs);

        // Be told when it terminates - however that happens. Keyed by instance
        // id, which termination_msg carries: names can be reused by a
        // relaunch, instance ids never are.
        cas::system::watch(self(), sys.ts);
        m_systems[sys.ts.get_actor()->instance_id()] = std::move(sys);
        std::cout << "  manager: launched " << msg.system_id << " with "
                  << msg.subscribers.size() << " stream(s)\n";
    }

    void on_unsubscribe(const unsubscribe& msg) {
        for (auto& [instance, sys] : m_systems) {
            auto it = sys.streams.find(msg.subscriber);
            if (sys.system_id != msg.system_id || it == sys.streams.end()) continue;

            // Tell the trading system to stop sending to it, then stop it.
            // Updates sent in between are dead letters - expected and harmless.
            trading_system::drop_stream drop;
            drop.stream = it->second;
            sys.ts.tell(drop);
            stop_without_blocking(it->second);
            sys.streams.erase(it);
            std::cout << "  manager: " << msg.subscriber << " unsubscribed from " << msg.system_id << "\n";
            return;
        }
    }

    // The trading system stopped - completed, failed, or stopped by someone
    // else. Either way its streams go with it.
    void on_terminated(const cas::termination_msg& msg) {
        auto it = m_systems.find(msg.instance_id);
        if (it == m_systems.end()) return;
        for (const auto& [subscriber, s] : it->second.streams) {
            stop_without_blocking(s);
        }
        std::cout << "  manager: retired " << it->second.system_id << " and its "
                  << it->second.streams.size() << " stream(s)\n";
        m_systems.erase(it);  // drop our references so the actors can be freed
        retired.fetch_add(1);
    }

    // From a handler, never wait for a stop: that would block this worker
    // thread (and every actor on it) until the other actor has finished.
    static void stop_without_blocking(const cas::actor_ref& ref) {
        cas::stop_config config;
        config.wait_for_stop = false;
        cas::system::stop_actor(ref, config);
    }

    std::unordered_map<size_t, running_system> m_systems;  // by instance id
};

void publish(const std::string& system_id, double price) {
    market_update update;
    update.price = price;
    trading_system::find(system_id).tell(update);
}

} // namespace

namespace tut {

bool run_trading_manager() {
    bool ok = true;

    // Handler exceptions are reported here (without a callback, to stderr).
    // The trading system's failure in part 4 lands in it.
    std::atomic<int> failures{0};
    cas::system::set_handler_exception_handler([&failures](const cas::handler_exception_info& info) {
        std::cout << "  [report] " << info.actor_name << ": " << info.what << "\n";
        failures.fetch_add(1);
    });

    heading("Starting the system with just the manager");
    auto manager = cas::system::create<trading_manager>();
    cas::system::start();
    const size_t baseline = cas::system::actor_count();  // the manager

    heading("1. Launching two trading systems at runtime");
    trading_manager::launch es;
    es.system_id = "es";
    es.subscribers = {"alice", "bob"};
    manager.tell(es);
    trading_manager::launch nq;
    nq.system_id = "nq";
    nq.subscribers = {"carol", "dave"};
    manager.tell(nq);

    ok &= check(wait_until([] { return trading_system::find("es").is_valid() && trading_system::find("nq").is_valid(); }),
                "both trading systems are running");
    ok &= check(cas::system::actor_count() == baseline + 6, "two trading systems and four streams were created");

    publish("es", 101.5);
    publish("nq", 202.25);
    ok &= check(wait_until([] {
                    for (const char* s : {"alice", "bob", "carol", "dave"}) {
                        if (stream::updates_for(s) != 1) return false;
                    }
                    return true;
                }),
                "each subscriber's stream received its system's update");

    heading("2. carol unsubscribes from nq; nq keeps running");
    trading_manager::unsubscribe leave;
    leave.system_id = "nq";
    leave.subscriber = "carol";
    manager.tell(leave);
    ok &= check(wait_until([] { return stream::stopped.load() == 1; }), "carol's stream was stopped");
    ok &= check(trading_system::find("nq").is_running(), "nq keeps running");

    heading("3. es completes its run");
    trading_system::find("es").tell(trading_system::run_complete{});
    ok &= check(wait_until([] { return trading_manager::retired.load() == 1; }), "the manager was told es terminated");
    ok &= check(wait_until([] { return stream::stopped.load() == 3; }), "alice's and bob's streams were stopped with it");
    ok &= check(!cas::actor_registry::exists(trading_system::name_for("es")), "es's name was released");

    heading("4. nq's handler throws");
    publish("nq", -1.0);  // on_update throws; on_handler_exception() says stop
    ok &= check(wait_until([&] { return failures.load() == 1; }), "the exception was reported, not fatal");
    ok &= check(wait_until([] { return trading_manager::retired.load() == 2; }), "the manager retired nq like a completed run");
    ok &= check(wait_until([] { return stream::stopped.load() == 4; }), "dave's stream was stopped with it");

    // Everything the manager created is gone; only the manager is left
    ok &= check(wait_until([&] { return cas::system::actor_count() == baseline; }), "only the manager remains");
    ok &= check(manager.is_running(), "the manager is still running");

    heading("Shutting down");
    cas::system::shutdown();
    cas::system::wait_for_shutdown();
    cas::system::clear_handler_exception_handler();
    return ok;
}

} // namespace tut
