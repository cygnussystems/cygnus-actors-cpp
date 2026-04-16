// Example usage of Cygnus Actor Framework
// This is a sketch of what the API should feel like

#include <iostream>
#include <string>
#include <sstream>
#include "cas/cas.h"

// Debug print helper - variadic template for easy printing
template<typename... Args>
void println(Args&&... args) {
    std::ostringstream oss;
    (oss << ... << std::forward<Args>(args));
    oss << '\n';
    std::cout << oss.str() << std::flush;  // Always flush!
}

// User-defined messages - use your own namespace (not cas::)
namespace message {
    struct ping : public cas::message_base {
        int id;
        std::string data;
    };

    struct pong : public cas::message_base {
        int id;
        std::string response;
    };
}

// Operation tags for RPC-style invocations
struct calculate_op {};
struct get_status_op {};

// User-defined actors - use your own namespace (not cas::)
namespace actor {

class pong : public cas::actor {
private:
    cas::actor_ref ping_actor_ref;  // Reference to ping actor (for replies)

protected:
    // Lifecycle hooks
    void on_start() override {
        println("[PONG] on_start() called");
        set_name("pong");

        // Register message handlers
        handler<message::ping>(&pong::on_ping);

        // Register invoke handlers (RPC-style)
        ask_handler<double, calculate_op>(&pong::do_calculate);

        println("[PONG] actor started");

        // Could look up other actors here if needed
        // ping_actor_ref = cas::actor_registry::get("ping");
    }

    void on_shutdown() override {
        println("[PONG] shutting down (can still send messages)");
    }

    void on_stop() override {
        println("[PONG] stopped");
    }

    // Message handlers - user writes these
    void on_ping(const message::ping& msg) {
        println("[PONG] Received ping: ", msg.id, " - ", msg.data);
        println("[PONG] msg.sender is_valid: ", msg.sender.is_valid() ? "true" : "false");

        // Send response back to sender
        message::pong response;
        response.id = msg.id;
        response.response = "pong response";

        if (msg.sender.is_valid()) {
            println("[PONG] Sending pong response to sender");
            msg.sender.tell(response);
            println("[PONG] Pong response sent");
        } else {
            println("[PONG] ERROR: msg.sender is invalid!");
        }
    }

    // RPC handler - just returns a value like a function
    double do_calculate(int value) {
        std::cout << "Calculating for value: " << value << "\n";
        return value * 2.0;
    }
};



class ping : public cas::actor {
private:
    cas::actor_ref pong_actor_ref;  // Holds reference to pong actor
    int ping_count = 0;

protected:
    void on_start() override {
        println("[PING] on_start() called");
        set_name("ping");

        // Register handler using lambda (alternative style)
        handler<message::pong>([this](const message::pong& msg) {
            on_pong(msg);
        });

        println("[PING] actor started, looking up pong actor");

        // Look up the pong actor by name from registry
        pong_actor_ref = cas::actor_registry::get("pong");

        if (!pong_actor_ref.is_valid()) {
            println("[PING] ERROR: pong actor not found in registry!");
            return;
        }

        println("[PING] found pong actor, sending initial ping");

        // Send initial ping
        message::ping ping_msg;
        ping_msg.id = ++ping_count;
        ping_msg.data = "hello from ping";
        pong_actor_ref.tell(ping_msg);

        println("[PING] initial ping sent");
    }

    void on_shutdown() override {
        println("[PING] shutting down");
    }

    void on_stop() override {
        println("[PING] stopped");
    }

    void on_pong(const message::pong& msg) {
        println("[PING] Received pong: ", msg.id, " - ", msg.response);

        if (ping_count < 3) {
            // Send another ping
            message::ping ping_msg;
            ping_msg.id = ++ping_count;
            ping_msg.data = "ping again";
            pong_actor_ref.tell(ping_msg);
            println("[PING] sent ping #", ping_count);
        } else {
            // TODO: Test invoke() when implemented
            // println("[PING] Invoking calculate on pong actor...");
            // double result = pong_actor_ref.invoke<double>(calculate_op{}, 42);
            // println("[PING] Got calculation result: ", result);

            // Done, stop the system
            println("[PING] Shutting down system...");
            cas::system::shutdown();
        }
    }
};

} // namespace actor

int main() {
    println("[MAIN] Starting program");

    // Create actors - system auto-manages them
    println("[MAIN] Creating pong actor...");
    auto pong = cas::system::create<actor::pong>();

    println("[MAIN] Creating ping actor...");
    auto ping = cas::system::create<actor::ping>();

    // Start the system
    println("[MAIN] Starting system...");
    cas::system::start();

    println("[MAIN] System started, waiting for shutdown...");

    // Wait for system to complete
    cas::system::wait_for_shutdown();

    println("[MAIN] System shutdown complete");
    return 0;
}
