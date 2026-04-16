#include "../test_common.h"

// Level 1: Send a single message to a single actor

namespace single_msg_test {
    struct ping : public cas::message_base {
        int value;
    };

    class receiver : public cas::actor {
    private:
        int received_count_ = 0;
        int last_value_ = 0;
        bool handler_was_called_ = false;

    protected:
        void on_start() override {
            set_name("receiver");
            handler<ping>(&receiver::on_ping);
        }

        void on_ping(const ping& msg) {
            handler_was_called_ = true;
            received_count_++;
            last_value_ = msg.value;
        }

    public:
        int get_received_count() const { return received_count_; }
        int get_last_value() const { return last_value_; }
        bool was_handler_called() const { return handler_was_called_; }
    };
}

TEST_CASE("Can send a single message to an actor", "[01_simple][messaging]") {
    auto receiver = cas::system::create<single_msg_test::receiver>();

    cas::system::start();
    wait_ms(50);

    auto receiver_ref = cas::actor_registry::get("receiver");
    REQUIRE(receiver_ref.is_valid());

    // Send one message
    single_msg_test::ping msg;
    msg.value = 42;
    receiver_ref.tell(msg);

    wait_ms(100);

    // Verify it was received
    auto& recv_actor = receiver.get_checked<single_msg_test::receiver>();
    REQUIRE(recv_actor.was_handler_called());
    REQUIRE(recv_actor.get_received_count() == 1);
    REQUIRE(recv_actor.get_last_value() == 42);

    TEST_CLEANUP();
}

TEST_CASE("Can send multiple messages to same actor", "[01_simple][messaging]") {
    auto receiver = cas::system::create<single_msg_test::receiver>();

    cas::system::start();
    wait_ms(50);

    auto receiver_ref = cas::actor_registry::get("receiver");
    REQUIRE(receiver_ref.is_valid());

    // Send 5 messages
    for (int i = 1; i <= 5; i++) {
        single_msg_test::ping msg;
        msg.value = i * 10;
        receiver_ref.tell(msg);
    }

    wait_ms(100);

    // Verify all were received
    auto& recv_actor = receiver.get_checked<single_msg_test::receiver>();
    REQUIRE(recv_actor.get_received_count() == 5);
    REQUIRE(recv_actor.get_last_value() == 50);  // Last message value

    TEST_CLEANUP();
}

TEST_CASE("Message sent from main has invalid sender", "[01_simple][messaging]") {
    class sender_checker : public cas::actor {
    private:
        bool sender_was_invalid_ = false;

    protected:
        void on_start() override {
            set_name("sender_checker");
            handler<single_msg_test::ping>(&sender_checker::on_ping);
        }

        void on_ping(const single_msg_test::ping& msg) {
            sender_was_invalid_ = !msg.sender.is_valid();
        }

    public:
        bool was_sender_invalid() const { return sender_was_invalid_; }
    };

    auto checker = cas::system::create<sender_checker>();

    cas::system::start();
    wait_ms(50);

    auto checker_ref = cas::actor_registry::get("sender_checker");
    REQUIRE(checker_ref.is_valid());

    // Send from main (no actor context)
    single_msg_test::ping msg;
    msg.value = 1;
    checker_ref.tell(msg);

    wait_ms(100);

    // Sender should be invalid
    auto& check_actor = checker.get_checked<sender_checker>();
    REQUIRE(check_actor.was_sender_invalid());

    TEST_CLEANUP();
}
