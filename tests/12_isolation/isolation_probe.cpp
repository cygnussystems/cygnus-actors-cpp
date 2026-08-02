#define CATCH_CONFIG_MAIN
#include "../test_common.h"

// Test-isolation probe. THIS BINARY IS EXPECTED TO FAIL.
//
// It is not part of unit_tests. It exists to prove, as a standing check rather
// than a manual experiment, that a failing test does not corrupt the tests that
// run after it.
//
// The actor system is a process-wide singleton. Before the RAII guards, a
// failing REQUIRE unwound past the cleanup at the end of the test body and left
// the system running; every subsequent test then failed too, because start()
// returns early when already running and no handlers get registered. One real
// failure produced a cascade of fake ones.
//
// This binary contains exactly one deliberately-failing test followed by
// several healthy ones that all start the system. Correct behaviour is
// EXACTLY ONE failing test case. Any more means isolation has regressed.
//
// Run it via tests/12_isolation/check_isolation.cmake, or by hand:
//     isolation_probe.exe --reporter compact
// and confirm the summary reads "Failed 1 test case".

namespace isolation_probe {
    struct ping : public cas::message_base { int value = 0; };

    class echo_actor : public cas::actor {
    public:
        std::atomic<int> received{0};
    protected:
        void on_start() override {
            set_name("echo_actor");
            handler<ping>([this](const ping&) { received.fetch_add(1); });
        }
    };

    // Drives one full start/message/assert cycle. Used by the healthy tests so
    // they exercise the same machinery the failing test leaves behind.
    void run_healthy_cycle() {
        // The direct precondition a leaked test breaks. Without this the probe
        // is insensitive: each cycle creates a fresh actor whose set_name()
        // overwrites the registry entry, so the cycle can appear to succeed
        // even when the previous test left the system running. Assert the
        // clean-slate precondition explicitly rather than inferring it.
        REQUIRE_FALSE(cas::system::is_running());
        REQUIRE(cas::system::actor_count() == 0);

        auto a = cas::system::create<echo_actor>();
        cas::system::start();
        wait_ms(50);

        auto ref = cas::actor_registry::get("echo_actor");
        REQUIRE(ref.is_valid());

        ref.tell(ping{});

        auto& obj = a.get_checked<echo_actor>();
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (obj.received.load() == 0 &&
               std::chrono::steady_clock::now() < deadline) {
            wait_ms(10);
        }
        REQUIRE(obj.received.load() == 1);
    }
}

// Catch2 runs test cases in declaration order within a file by default, so the
// failing case below runs before the healthy ones that follow it.

TEST_CASE("PROBE - deliberately failing test", "[12_isolation]") {
    CAS_TEST_GUARD();
    using namespace isolation_probe;

    auto a = cas::system::create<echo_actor>();
    cas::system::start();
    wait_ms(50);

    auto ref = cas::actor_registry::get("echo_actor");
    REQUIRE(ref.is_valid());
    ref.tell(ping{});
    wait_ms(50);

    auto& obj = a.get_checked<echo_actor>();

    // Intentional failure. Unwinds here, skipping everything below - including
    // any cleanup written at the end of the body. Only the RAII guard runs.
    REQUIRE(obj.received.load() == 999);

    TEST_CLEANUP();  // deliberately unreachable
}

TEST_CASE("Healthy test 1 after a failure", "[12_isolation]") {
    CAS_TEST_GUARD();
    isolation_probe::run_healthy_cycle();
}

TEST_CASE("Healthy test 2 after a failure", "[12_isolation]") {
    CAS_TEST_GUARD();
    isolation_probe::run_healthy_cycle();
}

TEST_CASE("Healthy test 3 after a failure", "[12_isolation]") {
    CAS_TEST_GUARD();
    isolation_probe::run_healthy_cycle();
}

TEST_CASE("Healthy test 4 after a failure - reconfigures the system",
          "[12_isolation]") {
    // configure() throws if the system is still running, so this additionally
    // proves the failing test did not leave it up.
    CAS_CONFIG_GUARD();

    cas::system_config config;
    config.queue_threshold = 500;
    cas::system::configure(config);

    isolation_probe::run_healthy_cycle();
}
