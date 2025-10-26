#include "../test_common.h"

// Level 0: Most basic test - can we even create an actor?

TEST_CASE("Can create a simple actor", "[00_basic][creation]") {
    class simple_actor : public cas::actor {
    protected:
        void on_start() override {}
    };

    auto actor_ref = cas::system::create<simple_actor>();

    // Verify actor was created
    REQUIRE(actor_ref.is_valid());

    TEST_CLEANUP();
}

TEST_CASE("Can create multiple actors", "[00_basic][creation]") {
    class simple_actor : public cas::actor {
    protected:
        void on_start() override {}
    };

    auto actor1 = cas::system::create<simple_actor>();
    auto actor2 = cas::system::create<simple_actor>();
    auto actor3 = cas::system::create<simple_actor>();

    // Verify all were created
    REQUIRE(actor1.is_valid());
    REQUIRE(actor2.is_valid());
    REQUIRE(actor3.is_valid());

    // Verify they're different actors
    REQUIRE(actor1 != actor2);
    REQUIRE(actor2 != actor3);
    REQUIRE(actor1 != actor3);

    TEST_CLEANUP();
}

TEST_CASE("Actor count is tracked correctly", "[00_basic][creation]") {
    class simple_actor : public cas::actor {
    protected:
        void on_start() override {}
    };

    size_t initial_count = cas::system::actor_count();
    REQUIRE(initial_count == 0);

    auto actor1 = cas::system::create<simple_actor>();
    REQUIRE(cas::system::actor_count() == 1);

    auto actor2 = cas::system::create<simple_actor>();
    REQUIRE(cas::system::actor_count() == 2);

    auto actor3 = cas::system::create<simple_actor>();
    REQUIRE(cas::system::actor_count() == 3);

    TEST_CLEANUP();
}

TEST_CASE("System starts and stops cleanly", "[00_basic][system]") {
    class simple_actor : public cas::actor {
    protected:
        void on_start() override {}
    };

    auto actor = cas::system::create<simple_actor>();

    REQUIRE(!cas::system::is_running());

    cas::system::start();
    REQUIRE(cas::system::is_running());

    wait_ms(50);

    cas::system::shutdown();
    cas::system::wait_for_shutdown();
    REQUIRE(!cas::system::is_running());

    TEST_CLEANUP();
}
