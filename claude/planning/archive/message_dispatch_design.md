# Message Dispatch Design

## Goal
Automatically call the correct typed `on_message()` handler when a message arrives, without requiring manual registration.

## Approach: Type Registry with Template Magic

### Core Idea
1. Each actor maintains a map: `std::type_index -> handler function`
2. Use templates to automatically detect which `on_message()` overloads exist
3. Register them at construction time
4. At dispatch time, look up the handler by message type

### Implementation Sketch

```cpp
// In actor.h
class actor {
private:
    // Map from message type to handler function
    std::unordered_map<std::type_index, std::function<void(message_base*)>> handlers_;

protected:
    // User overrides this for each message type they want to handle
    // These are NOT virtual - they're compile-time overloads
    // e.g.: void on_message(const ping_message& msg) { ... }

    // Helper to register a handler for a specific message type
    template<typename MessageType>
    void register_handler() {
        handlers_[typeid(MessageType)] = [this](message_base* base_msg) {
            // Cast to concrete type and call user's on_message
            MessageType* msg = static_cast<MessageType*>(base_msg);
            this->on_message(*msg);
        };
    }

public:
    // Internal: dispatch message to correct handler
    void dispatch_message(message_base* msg) {
        auto it = handlers_.find(typeid(*msg));
        if (it != handlers_.end()) {
            it->second(msg);  // Call the registered handler
        } else {
            // No handler for this message type
            on_unhandled_message(msg);
        }
    }

    virtual void on_unhandled_message(message_base* msg) {
        // Default: ignore or log warning
    }
};
```

### User Code

```cpp
class my_actor : public actor {
protected:
    void on_start() override {
        set_name("my_actor");

        // Manual registration (not ideal, but works)
        register_handler<ping_message>();
        register_handler<pong_message>();
    }

    // User writes these - NOT virtual
    void on_message(const ping_message& msg) {
        // Handle ping
    }

    void on_message(const pong_message& msg) {
        // Handle pong
    }
};
```

## Problem: Manual Registration is Ugly

We want automatic registration. Options:

### Option A: CRTP + Template Detection
Use CRTP to detect which `on_message()` overloads exist at compile time and auto-register them.

This is complex but doable with C++17 `if constexpr` and SFINAE.

### Option B: Macro Helper
```cpp
class my_actor : public actor {
protected:
    void on_start() override {
        set_name("my_actor");
        REGISTER_HANDLERS(ping_message, pong_message);
    }

    HANDLE_MESSAGE(ping_message) {
        // Handle ping
    }
};
```

Not as clean but very explicit.

### Option C: Constructor Registration
Actor constructor could take a list of message types to register.

## Recommendation
Start with manual registration in `on_start()` for simplicity. We can add automatic detection later as an optimization/convenience.

The key is that the mechanism is in place and users just need to call `register_handler<T>()` for now.
