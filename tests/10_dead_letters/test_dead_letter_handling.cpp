#include <catch2/catch.hpp>
#include "cas/cas.h"
#include <atomic>
#include <thread>
#include <chrono>

// Simple message for testing
struct test_msg : cas::message_base {
    int value = 0;
};

// Basic actor that just accepts messages
class basic_actor : public cas::actor {
protected:
    void on_start() override {
        set_name("basic");
        handler<test_msg>([this](const test_msg& msg) {
            m_received++;
        });
    }

public:
    std::atomic<int> m_received{0};
};

TEST_CASE("Dead letter stats track dropped messages", "[10_dead_letters][stats]") {
    // Reset stats before test
    cas::system::reset_dead_letter_stats();

    // Verify initial stats are zero
    auto stats = cas::system::get_dead_letter_stats();
    REQUIRE(stats.dropped_tell == 0);
    REQUIRE(stats.dropped_ask == 0);

    // Create and start system
    auto actor = cas::system::create<basic_actor>();
    cas::system::start();

    // Wait for actor to be ready
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Send a message to verify it works
    test_msg msg1; msg1.value = 1;
    actor.tell(msg1);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Stop the actor
    cas::system::stop_actor(actor);

    // Now send messages to the stopped actor
    test_msg msg2; msg2.value = 2;
    test_msg msg3; msg3.value = 3;
    test_msg msg4; msg4.value = 4;
    actor.tell(msg2);
    actor.tell(msg3);
    actor.tell(msg4);

    // Check stats - should have 3 dropped messages
    stats = cas::system::get_dead_letter_stats();
    REQUIRE(stats.dropped_tell == 3);
    REQUIRE(stats.dropped_ask == 0);

    // Cleanup
    cas::system::shutdown();
    cas::system::wait_for_shutdown();
    cas::system::reset();
}

TEST_CASE("Dead letter stats can be reset", "[10_dead_letters][stats]") {
    // First generate some dead letters
    cas::system::reset_dead_letter_stats();

    auto actor = cas::system::create<basic_actor>();
    cas::system::start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    cas::system::stop_actor(actor);

    // Send messages to stopped actor
    test_msg m1, m2;
    actor.tell(m1);
    actor.tell(m2);

    auto stats = cas::system::get_dead_letter_stats();
    REQUIRE(stats.dropped_tell == 2);

    // Reset stats
    cas::system::reset_dead_letter_stats();

    stats = cas::system::get_dead_letter_stats();
    REQUIRE(stats.dropped_tell == 0);
    REQUIRE(stats.dropped_ask == 0);

    cas::system::shutdown();
    cas::system::wait_for_shutdown();
    cas::system::reset();
}

TEST_CASE("Dead letter callback is invoked", "[10_dead_letters][callback]") {
    cas::system::reset_dead_letter_stats();

    std::atomic<int> callback_count{0};
    std::string last_actor_name;

    // Set up callback handler
    cas::system::set_dead_letter_handler([&](const cas::dead_letter_info& info) {
        callback_count++;
        last_actor_name = info.target_actor_name;
    });

    auto actor = cas::system::create<basic_actor>();
    cas::system::start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    cas::system::stop_actor(actor);

    // Send messages to stopped actor
    test_msg cb_msg1, cb_msg2;
    actor.tell(cb_msg1);
    actor.tell(cb_msg2);

    // Give callback time to execute
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    REQUIRE(callback_count == 2);
    REQUIRE(last_actor_name == "basic");

    // Clear handler
    cas::system::clear_dead_letter_handler();

    // Send another message - callback should not be invoked
    test_msg cb_msg3;
    actor.tell(cb_msg3);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    REQUIRE(callback_count == 2);  // Should still be 2

    cas::system::shutdown();
    cas::system::wait_for_shutdown();
    cas::system::reset();
}

TEST_CASE("Dead letter info contains correct message details", "[10_dead_letters][callback]") {
    cas::system::reset_dead_letter_stats();

    uint64_t captured_message_id = 0;
    cas::actor_state captured_state;

    cas::system::set_dead_letter_handler([&](const cas::dead_letter_info& info) {
        captured_message_id = info.message_id;
        captured_state = info.target_state;
    });

    auto actor = cas::system::create<basic_actor>();
    cas::system::start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    cas::system::stop_actor(actor);

    // Send a message
    test_msg info_msg;
    info_msg.value = 42;
    actor.tell(info_msg);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Message ID should be non-zero
    REQUIRE(captured_message_id > 0);

    // State should be stopped (actor was fully stopped)
    REQUIRE(captured_state == cas::actor_state::stopped);

    cas::system::clear_dead_letter_handler();
    cas::system::shutdown();
    cas::system::wait_for_shutdown();
    cas::system::reset();
}
