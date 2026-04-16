#include "../test_common.h"
#include "cas/cas.h"
#include <thread>
#include <chrono>

using namespace std::chrono_literals;

namespace actor {

// Simple test actor
class test_actor : public cas::actor {
protected:
    void on_start() override {
        set_name("test_actor");
    }
};

} // namespace actor

namespace message {
struct ping : public cas::message_base {};
} // namespace message

TEST_CASE("Basic actor removal", "[07_dynamic_removal][basic]") {
    cas::system::reset();
    cas::system::configure(cas::system_config{});

    SECTION("Can stop a single actor synchronously") {
        auto ref = cas::system::create<actor::test_actor>();
        cas::system::start();

        // Wait a bit for actor to start
        std::this_thread::sleep_for(50ms);

        // Actor should be running
        REQUIRE(cas::system::is_actor_running(ref));
        REQUIRE(ref.is_running());

        // Stop the actor
        bool stopped = cas::system::stop_actor(ref);
        REQUIRE(stopped == true);

        // Actor should no longer be running
        REQUIRE_FALSE(cas::system::is_actor_running(ref));
        REQUIRE_FALSE(ref.is_running());

        // Should not be in registry
        REQUIRE_FALSE(cas::actor_registry::exists("test_actor"));

        cas::system::shutdown();
        cas::system::wait_for_shutdown();
    }

    SECTION("Stop non-existent actor returns false") {
        cas::system::start();

        // Try to stop by invalid name
        bool stopped = cas::system::stop_actor("nonexistent");
        REQUIRE(stopped == false);

        // Try to stop by invalid ref
        cas::actor_ref invalid_ref;
        stopped = cas::system::stop_actor(invalid_ref);
        REQUIRE(stopped == false);

        cas::system::shutdown();
        cas::system::wait_for_shutdown();
    }

    SECTION("Stop already-stopped actor returns false") {
        auto ref = cas::system::create<actor::test_actor>();
        cas::system::start();

        std::this_thread::sleep_for(50ms);

        // Stop once - should succeed
        bool stopped = cas::system::stop_actor(ref);
        REQUIRE(stopped == true);

        // Stop again - should fail
        stopped = cas::system::stop_actor(ref);
        REQUIRE(stopped == false);

        cas::system::shutdown();
        cas::system::wait_for_shutdown();
    }

    SECTION("Can stop actor by name") {
        auto ref = cas::system::create<actor::test_actor>();
        cas::system::start();

        std::this_thread::sleep_for(50ms);

        // Verify actor is in registry
        REQUIRE(cas::actor_registry::exists("test_actor"));

        // Stop by name
        bool stopped = cas::system::stop_actor("test_actor");
        REQUIRE(stopped == true);

        // Should be removed from registry
        REQUIRE_FALSE(cas::actor_registry::exists("test_actor"));

        cas::system::shutdown();
        cas::system::wait_for_shutdown();
    }

    SECTION("Remaining actors continue working after one is stopped") {
        auto actor1 = cas::system::create<actor::test_actor>();
        auto actor2 = cas::system::create<actor::test_actor>();
        auto actor3 = cas::system::create<actor::test_actor>();

        cas::system::start();
        std::this_thread::sleep_for(50ms);

        // All should be running
        REQUIRE(cas::system::is_actor_running(actor1));
        REQUIRE(cas::system::is_actor_running(actor2));
        REQUIRE(cas::system::is_actor_running(actor3));

        // Stop actor2
        bool stopped = cas::system::stop_actor(actor2);
        REQUIRE(stopped == true);

        // Actor 1 and 3 should still be running
        REQUIRE(cas::system::is_actor_running(actor1));
        REQUIRE_FALSE(cas::system::is_actor_running(actor2));
        REQUIRE(cas::system::is_actor_running(actor3));

        cas::system::shutdown();
        cas::system::wait_for_shutdown();
    }
}
