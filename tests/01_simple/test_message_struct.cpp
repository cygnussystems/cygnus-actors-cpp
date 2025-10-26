#include "../test_common.h"

// Level 1: Test message structure basics

namespace test_msg {
    struct simple : public cas::message_base {
        int value;
    };

    struct with_string : public cas::message_base {
        std::string data;
    };

    struct multi_field : public cas::message_base {
        int id;
        std::string name;
        double value;
    };
}

TEST_CASE("Can create a simple message", "[01_simple][message]") {
    test_msg::simple msg;
    msg.value = 42;

    REQUIRE(msg.value == 42);
}

TEST_CASE("Message has default invalid sender", "[01_simple][message]") {
    test_msg::simple msg;

    REQUIRE(!msg.sender.is_valid());
}

TEST_CASE("Can create message with string", "[01_simple][message]") {
    test_msg::with_string msg;
    msg.data = "hello world";

    REQUIRE(msg.data == "hello world");
}

TEST_CASE("Can create message with multiple fields", "[01_simple][message]") {
    test_msg::multi_field msg;
    msg.id = 123;
    msg.name = "test";
    msg.value = 3.14;

    REQUIRE(msg.id == 123);
    REQUIRE(msg.name == "test");
    REQUIRE(msg.value == 3.14);
}

TEST_CASE("Messages are copyable", "[01_simple][message]") {
    test_msg::multi_field msg1;
    msg1.id = 100;
    msg1.name = "original";
    msg1.value = 1.5;

    test_msg::multi_field msg2 = msg1;

    REQUIRE(msg2.id == 100);
    REQUIRE(msg2.name == "original");
    REQUIRE(msg2.value == 1.5);

    // Modify copy
    msg2.id = 200;
    msg2.name = "modified";

    // Original unchanged
    REQUIRE(msg1.id == 100);
    REQUIRE(msg1.name == "original");
}
