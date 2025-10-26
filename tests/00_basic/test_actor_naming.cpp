#include "../test_common.h"

// Level 0: Test actor naming functionality

TEST_CASE("Actor has auto-generated name by default", "[00_basic][naming]") {
    class simple_actor : public cas::actor {
    protected:
        void on_start() override {}
    };

    auto actor_ref = cas::system::create<simple_actor>();

    // Name should be auto-generated as typename_id
    REQUIRE(!actor_ref.name().empty());
    REQUIRE(actor_ref.name().find("_1") != std::string::npos);  // Should end with _1 (first instance)

    TEST_CLEANUP();
}

TEST_CASE("Actor can set name in on_start", "[00_basic][naming]") {
    class named_actor : public cas::actor {
    protected:
        void on_start() override {
            set_name("test_actor");
        }
    };

    auto actor_ref = cas::system::create<named_actor>();

    cas::system::start();
    wait_ms(50);

    REQUIRE(actor_ref.name() == "test_actor");

    TEST_CLEANUP();
}

TEST_CASE("Multiple actors can have different names", "[00_basic][naming]") {
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

    auto actor1_ref = cas::system::create<named_actor>("actor_one");
    auto actor2_ref = cas::system::create<named_actor>("actor_two");
    auto actor3_ref = cas::system::create<named_actor>("actor_three");

    cas::system::start();
    wait_ms(50);

    REQUIRE(actor1_ref.name() == "actor_one");
    REQUIRE(actor2_ref.name() == "actor_two");
    REQUIRE(actor3_ref.name() == "actor_three");

    TEST_CLEANUP();
}
