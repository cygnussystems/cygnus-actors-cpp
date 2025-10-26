#include "../test_common.h"

// Level 1: Test handler registration (no actual messaging yet)

namespace handler_test {
    struct test_message : public cas::message_base {
        int value;
    };

    class handler_actor : public cas::actor {
    private:
        int handler_call_count_ = 0;
        int last_value_ = 0;

    protected:
        void on_start() override {
            set_name("handler_test");

            // Register handler using member function pointer
            handler<test_message>(&handler_actor::on_test_message);
        }

        void on_test_message(const test_message& msg) {
            handler_call_count_++;
            last_value_ = msg.value;
        }

    public:
        int get_handler_call_count() const { return handler_call_count_; }
        int get_last_value() const { return last_value_; }
    };

    class lambda_handler_actor : public cas::actor {
    private:
        int handler_call_count_ = 0;

    protected:
        void on_start() override {
            set_name("lambda_handler");

            // Register handler using lambda
            handler<test_message>([this](const test_message& msg) {
                handler_call_count_++;
            });
        }

    public:
        int get_handler_call_count() const { return handler_call_count_; }
    };

    class multi_handler_actor : public cas::actor {
    private:
        int msg1_count_ = 0;
        int msg2_count_ = 0;

    protected:
        void on_start() override {
            set_name("multi_handler");

            handler<test_message>(&multi_handler_actor::on_test);
            handler<cas::message_base>(&multi_handler_actor::on_base);
        }

        void on_test(const test_message& msg) {
            msg1_count_++;
        }

        void on_base(const cas::message_base& msg) {
            msg2_count_++;
        }

    public:
        int get_msg1_count() const { return msg1_count_; }
        int get_msg2_count() const { return msg2_count_; }
    };
}

TEST_CASE("Actor can register a handler", "[01_simple][handler]") {
    auto actor = cas::system::create<handler_test::handler_actor>();

    cas::system::start();
    wait_ms(50);

    // Just verify actor started (handler registration happens in on_start)
    REQUIRE(actor.name() == "handler_test");

    TEST_CLEANUP();
}

TEST_CASE("Actor can register lambda handler", "[01_simple][handler]") {
    auto actor = cas::system::create<handler_test::lambda_handler_actor>();

    cas::system::start();
    wait_ms(50);

    REQUIRE(actor.name() == "lambda_handler");

    TEST_CLEANUP();
}

TEST_CASE("Actor can register multiple handlers", "[01_simple][handler]") {
    auto actor = cas::system::create<handler_test::multi_handler_actor>();

    cas::system::start();
    wait_ms(50);

    REQUIRE(actor.name() == "multi_handler");

    TEST_CLEANUP();
}
