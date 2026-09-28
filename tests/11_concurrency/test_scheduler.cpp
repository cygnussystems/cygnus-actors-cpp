#include "../test_common.h"
#include <atomic>
#include <chrono>
#include <thread>

// Level 11: the ready-queue scheduler.
//
// Workers run only actors on their ready queue; an actor is queued when a push
// finds its mailbox idle, or when a turn ends with messages left. These tests
// pin down the properties that design has to keep:
//  - no message is left behind by a lost wake-up (workers park and must be
//    woken reliably)
//  - a busy actor yields its worker after max_messages_per_turn, so other
//    actors on the same worker are not starved
//  - an actor created at runtime cannot receive a message while its
//    on_start() is still running (INVARIANT 1) - previously it was listed on
//    its worker before on_start() ran, and a reply to something on_start()
//    sent could be handled concurrently with it

namespace scheduler_test {

struct ball : cas::message_base {
    int remaining = 0;
    bool pause = false;
};

std::atomic<int> g_volleys{0};

struct serve : cas::message_base {
    cas::actor_ref opponent;
    int volleys = 0;
};

// Returns every ball to whoever hit it. Every 50th hit it pauses first, so
// both workers run dry and park, and the next hit has to wake one of them.
class rally_player : public cas::actor {
protected:
    void on_start() override {
        handler<serve>([](const serve& s) {
            ball first;
            first.remaining = s.volleys;
            s.opponent.tell(first);
        });
        handler<ball>([](const ball& b) {
            g_volleys.fetch_add(1);
            if (b.remaining == 0) return;
            if (b.pause) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            ball next;
            next.remaining = b.remaining - 1;
            next.pause = (next.remaining % 50) == 0;
            b.sender.tell(next);
        });
    }
};

struct work : cas::message_base {};
struct probe : cas::message_base {};

std::atomic<int> g_flood_handled{0};
std::atomic<int> g_flood_handled_when_probed{-1};

class flooded : public cas::actor {
protected:
    void on_start() override {
        handler<work>([](const work&) {
            g_flood_handled.fetch_add(1);
            // Make each message cost something, so the flood outlasts one turn
            volatile int spin = 0;
            for (int i = 0; i < 2000; ++i) spin = spin + i;
        });
    }
};

class neighbour : public cas::actor {
protected:
    void on_start() override {
        handler<probe>([](const probe&) {
            g_flood_handled_when_probed.store(g_flood_handled.load());
        });
    }
};

struct ping : cas::message_base {};
struct pong : cas::message_base {};

class echo : public cas::actor {
protected:
    void on_start() override {
        handler<ping>([](const ping& p) { p.sender.tell(pong{}); });
    }
};

std::atomic<bool> g_in_on_start{false};
std::atomic<bool> g_pong_during_on_start{false};
std::atomic<bool> g_pong_handled{false};

// Created while the system runs, from a handler. Its on_start() sends a ping
// and then stays busy; the pong must not be handled until on_start() returns.
class slow_starter : public cas::actor {
public:
    explicit slow_starter(cas::actor_ref echo_ref) : m_echo(std::move(echo_ref)) {}

protected:
    void on_start() override {
        g_in_on_start = true;
        handler<pong>([](const pong&) {
            if (g_in_on_start.load()) g_pong_during_on_start = true;
            g_pong_handled = true;
        });
        m_echo.tell(ping{});
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        g_in_on_start = false;
    }

private:
    cas::actor_ref m_echo;
};

struct spawn_slow_starter : cas::message_base {
    cas::actor_ref echo_ref;
};

class spawner : public cas::actor {
protected:
    void on_start() override {
        handler<spawn_slow_starter>([](const spawn_slow_starter& msg) {
            cas::system::create<slow_starter>(msg.echo_ref);
        });
    }
};

template<typename Pred>
bool wait_for(Pred pred, int timeout_ms = 10000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

} // namespace scheduler_test

using namespace scheduler_test;

TEST_CASE("Workers that park are always woken: long rally with idle gaps",
          "[11_concurrency][scheduler]") {
    CAS_TEST_GUARD();
    g_volleys = 0;
    auto a = cas::system::create<rally_player>();
    auto b = cas::system::create<rally_player>();
    cas::system::start();

    // a serves to b; from then on each returns the ball to its sender.
    // volleys + 1 hits in total (remaining counts down to 0). A lost wake-up
    // stalls the rally and fails on the timeout.
    constexpr int volleys = 20'000;
    serve s;
    s.opponent = b;
    s.volleys = volleys;
    a.tell(s);

    REQUIRE(wait_for([] { return g_volleys.load() == volleys + 1; }, 60'000));
}

TEST_CASE("A busy actor yields its worker after max_messages_per_turn",
          "[11_concurrency][scheduler]") {
    CAS_CONFIG_GUARD();
    cas::system_config config;
    config.thread_pool_size = 1;  // both actors share one worker
    config.max_messages_per_turn = 64;
    cas::system::configure(config);

    g_flood_handled = 0;
    g_flood_handled_when_probed = -1;
    auto busy = cas::system::create<flooded>();
    auto other = cas::system::create<neighbour>();

    // Queue everything before start(), so both actors are ready from the first
    // moment and the only thing deciding who runs when is the scheduler.
    constexpr int flood = 20'000;
    for (int i = 0; i < flood; ++i) busy.tell(work{});
    other.tell(probe{});
    cas::system::start();

    REQUIRE(wait_for([] { return g_flood_handled_when_probed.load() >= 0; }));
    REQUIRE(wait_for([] { return g_flood_handled.load() == flood; }, 60'000));
    // busy was queued first, used one turn, and went behind the neighbour.
    INFO("flood messages handled before the neighbour ran: " << g_flood_handled_when_probed.load());
    REQUIRE(g_flood_handled_when_probed.load() <= 64);
}

TEST_CASE("max_messages_per_turn = 0 means no limit, and nothing is lost",
          "[11_concurrency][scheduler]") {
    CAS_CONFIG_GUARD();
    cas::system_config config;
    config.thread_pool_size = 1;
    config.max_messages_per_turn = 0;
    cas::system::configure(config);

    g_flood_handled = 0;
    g_flood_handled_when_probed = -1;
    auto busy = cas::system::create<flooded>();
    auto other = cas::system::create<neighbour>();

    constexpr int flood = 5'000;
    for (int i = 0; i < flood; ++i) busy.tell(work{});
    other.tell(probe{});
    cas::system::start();

    REQUIRE(wait_for([] { return g_flood_handled.load() == flood; }, 60'000));
    REQUIRE(wait_for([] { return g_flood_handled_when_probed.load() >= 0; }));
    // Unlimited: busy drained its whole mailbox in its first turn.
    REQUIRE(g_flood_handled_when_probed.load() == flood);
}

TEST_CASE("A message cannot be handled while the actor's on_start() is running",
          "[11_concurrency][scheduler]") {
    CAS_TEST_GUARD();
    g_in_on_start = false;
    g_pong_during_on_start = false;
    g_pong_handled = false;

    auto echo_ref = cas::system::create<echo>();
    auto spawner_ref = cas::system::create<spawner>();
    cas::system::start();

    // The slow_starter is created from a handler, while the system runs. Its
    // on_start() pings echo and then stays busy for 100ms; echo's pong arrives
    // long before that ends.
    spawn_slow_starter msg;
    msg.echo_ref = echo_ref;
    spawner_ref.tell(msg);

    REQUIRE(wait_for([] { return g_pong_handled.load(); }));
    REQUIRE_FALSE(g_pong_during_on_start.load());
}
