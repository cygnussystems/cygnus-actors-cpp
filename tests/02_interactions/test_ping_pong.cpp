#include "../test_common.h"

// Level 2: Two actors communicating

namespace ping_pong_test {
    struct ping : public cas::message_base {
        int count;
    };

    struct pong : public cas::message_base {
        int count;
    };

    class pong_actor : public cas::actor {
    private:
        int pings_received_ = 0;

    protected:
        void on_start() override {
            set_name("pong");
            handler<ping>(&pong_actor::on_ping);
        }

        void on_ping(const ping& msg) {
            pings_received_++;

            // Reply with pong
            if (msg.sender.is_valid()) {
                pong reply;
                reply.count = msg.count;
                msg.sender.tell(reply);
            }
        }

    public:
        int get_pings_received() const { return pings_received_; }
    };

    class ping_actor : public cas::actor {
    private:
        int pongs_received_ = 0;
        int max_count_;

    public:
        ping_actor(int max_count) : max_count_(max_count) {}

    protected:
        void on_start() override {
            set_name("ping");
            handler<pong>(&ping_actor::on_pong);

            // Send initial ping
            auto pong_ref = cas::actor_registry::get("pong");
            if (pong_ref.is_valid()) {
                ping msg;
                msg.count = 1;
                pong_ref.tell(msg);
            }
        }

        void on_pong(const pong& msg) {
            pongs_received_++;

            // Send another ping if not done
            if (msg.count < max_count_) {
                auto pong_ref = cas::actor_registry::get("pong");
                if (pong_ref.is_valid()) {
                    ping next_msg;
                    next_msg.count = msg.count + 1;
                    pong_ref.tell(next_msg);
                }
            }
        }

    public:
        int get_pongs_received() const { return pongs_received_; }
    };
}

TEST_CASE("Two actors can exchange one message", "[02_interactions][ping_pong]") {
    auto pong = cas::system::create<ping_pong_test::pong_actor>();
    auto ping = cas::system::create<ping_pong_test::ping_actor>(1);

    cas::system::start();
    wait_ms(200);

    auto& pong_actor = pong.get_checked<ping_pong_test::pong_actor>();
    auto& ping_actor = ping.get_checked<ping_pong_test::ping_actor>();
    REQUIRE(pong_actor.get_pings_received() == 1);
    REQUIRE(ping_actor.get_pongs_received() == 1);

    TEST_CLEANUP();
}

TEST_CASE("Two actors can exchange multiple messages", "[02_interactions][ping_pong]") {
    auto pong = cas::system::create<ping_pong_test::pong_actor>();
    auto ping = cas::system::create<ping_pong_test::ping_actor>(5);

    cas::system::start();
    wait_ms(300);

    auto& pong_actor = pong.get_checked<ping_pong_test::pong_actor>();
    auto& ping_actor = ping.get_checked<ping_pong_test::ping_actor>();
    REQUIRE(pong_actor.get_pings_received() == 5);
    REQUIRE(ping_actor.get_pongs_received() == 5);

    TEST_CLEANUP();
}

TEST_CASE("Sender field is correctly set in actor-to-actor messages", "[02_interactions][sender]") {
    class sender_validator : public cas::actor {
    private:
        bool sender_was_valid_ = false;

    protected:
        void on_start() override {
            set_name("validator");
            handler<ping_pong_test::ping>(&sender_validator::on_ping);
        }

        void on_ping(const ping_pong_test::ping& msg) {
            sender_was_valid_ = msg.sender.is_valid();
        }

    public:
        bool was_sender_valid() const { return sender_was_valid_; }
    };

    class sender_actor : public cas::actor {
    protected:
        void on_start() override {
            set_name("sender");

            auto validator_ref = cas::actor_registry::get("validator");
            if (validator_ref.is_valid()) {
                ping_pong_test::ping msg;
                msg.count = 1;
                validator_ref.tell(msg);
            }
        }
    };

    auto validator = cas::system::create<sender_validator>();
    auto sender = cas::system::create<sender_actor>();

    cas::system::start();
    wait_ms(150);

    auto& valid_actor = validator.get_checked<sender_validator>();
    REQUIRE(valid_actor.was_sender_valid());

    TEST_CLEANUP();
}
