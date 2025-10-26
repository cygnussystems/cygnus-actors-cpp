#include "../test_common.h"

// Level 3: Test message draining during shutdown

namespace draining_test {
    struct test_msg : public cas::message_base {
        int id;
    };

    class message_counter : public cas::actor {
    private:
        std::atomic<int>* count_;

    public:
        message_counter(std::atomic<int>* count) : count_(count) {}

    protected:
        void on_start() override {
            set_name("counter");
            handler<test_msg>(&message_counter::on_msg);
        }

        void on_msg(const test_msg& msg) {
            count_->fetch_add(1);
        }
    };
}

TEST_CASE("Messages are drained during shutdown", "[03_lifecycle][draining]") {
    std::atomic<int> message_count{0};

    auto actor = cas::system::create<draining_test::message_counter>(&message_count);

    cas::system::start();
    wait_ms(50);

    auto actor_ref = cas::actor_registry::get("counter");
    REQUIRE(actor_ref.is_valid());

    // Send 10 messages quickly
    for (int i = 0; i < 10; ++i) {
        draining_test::test_msg msg;
        msg.id = i;
        actor_ref.receive(msg);
    }

    // Shutdown immediately (messages may still be in queue)
    cas::system::shutdown();
    cas::system::wait_for_shutdown();

    // All messages should have been drained and processed
    REQUIRE(message_count.load() == 10);

    TEST_CLEANUP();
}

TEST_CASE("New messages rejected during shutdown", "[03_lifecycle][draining]") {
    std::atomic<int> message_count{0};

    auto actor = cas::system::create<draining_test::message_counter>(&message_count);

    cas::system::start();
    wait_ms(50);

    auto actor_ref = cas::actor_registry::get("counter");
    REQUIRE(actor_ref.is_valid());

    // Send one message
    draining_test::test_msg msg1;
    msg1.id = 1;
    actor_ref.receive(msg1);

    wait_ms(50);

    // Initiate shutdown
    cas::system::shutdown();

    // Try to send another message (should be dropped)
    draining_test::test_msg msg2;
    msg2.id = 2;
    actor_ref.receive(msg2);

    cas::system::wait_for_shutdown();

    // Only first message should have been processed
    REQUIRE(message_count.load() == 1);

    TEST_CLEANUP();
}
