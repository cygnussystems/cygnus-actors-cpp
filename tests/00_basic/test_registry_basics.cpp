#include "../test_common.h"

// Level 0: Test basic registry operations

TEST_CASE("Registry lookup of non-existent actor returns invalid ref", "[00_basic][registry]") {
    cas::system::start();
    wait_ms(50);

    auto actor_ref = cas::actor_registry::get("does_not_exist");

    REQUIRE(!actor_ref.is_valid());

    TEST_CLEANUP();
}

TEST_CASE("Actor can be registered and found by name", "[00_basic][registry]") {
    class named_actor : public cas::actor {
    protected:
        void on_start() override {
            set_name("findme");
        }
    };

    auto actor = cas::system::create<named_actor>();

    cas::system::start();
    wait_ms(50);

    auto actor_ref = cas::actor_registry::get("findme");

    REQUIRE(actor_ref.is_valid());

    TEST_CLEANUP();
}

TEST_CASE("Registry lookup before start returns invalid ref", "[00_basic][registry]") {
    class named_actor : public cas::actor {
    protected:
        void on_start() override {
            set_name("test_actor");
        }
    };

    auto actor = cas::system::create<named_actor>();

    // Try lookup before start (name not set yet)
    auto actor_ref = cas::actor_registry::get("test_actor");
    REQUIRE(!actor_ref.is_valid());

    // Start system
    cas::system::start();
    wait_ms(50);

    // Now it should be found
    actor_ref = cas::actor_registry::get("test_actor");
    REQUIRE(actor_ref.is_valid());

    TEST_CLEANUP();
}

TEST_CASE("Multiple actors with different names can be registered", "[00_basic][registry]") {
    class named_actor : public cas::actor {
    private:
        std::string name_to_set_;
    public:
        named_actor(const std::string& name) : name_to_set_(name) {}
    protected:
        void on_start() override {
            set_name(name_to_set_);
        }
    };

    auto actor1 = cas::system::create<named_actor>("actor_one");
    auto actor2 = cas::system::create<named_actor>("actor_two");
    auto actor3 = cas::system::create<named_actor>("actor_three");

    cas::system::start();
    wait_ms(50);

    auto ref1 = cas::actor_registry::get("actor_one");
    auto ref2 = cas::actor_registry::get("actor_two");
    auto ref3 = cas::actor_registry::get("actor_three");

    REQUIRE(ref1.is_valid());
    REQUIRE(ref2.is_valid());
    REQUIRE(ref3.is_valid());

    // Verify they're different
    REQUIRE(ref1 != ref2);
    REQUIRE(ref2 != ref3);
    REQUIRE(ref1 != ref3);

    TEST_CLEANUP();
}

TEST_CASE("Registry count is correct", "[00_basic][registry]") {
    class named_actor : public cas::actor {
    private:
        std::string name_to_set_;
    public:
        named_actor(const std::string& name) : name_to_set_(name) {}
    protected:
        void on_start() override {
            set_name(name_to_set_);
        }
    };

    REQUIRE(cas::actor_registry::count() == 0);

    auto actor1 = cas::system::create<named_actor>("one");
    REQUIRE(cas::actor_registry::count() == 0);  // Not registered until on_start

    cas::system::start();
    wait_ms(50);

    REQUIRE(cas::actor_registry::count() == 1);

    TEST_CLEANUP();
    REQUIRE(cas::actor_registry::count() == 0);  // Should be cleared after reset
}
