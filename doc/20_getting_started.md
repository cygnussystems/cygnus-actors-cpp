# Getting Started with Cygnus

This guide will help you build your first actor-based application using the Cygnus Actor Framework.

## Prerequisites

- C++17 compatible compiler
  - MSVC 2017 or later (Windows)
  - GCC 7+ or Clang 5+ (Linux/Mac)
- CMake 3.15 or later
- Windows: Visual Studio 2022 (for MSVC toolchain)

## Building Cygnus

### Windows (Visual Studio)

```cmd
# Using the provided build script
cd C:\path\to\CYGNUS_ACTOR_FRAMEWORK
.\claude\build.bat

# Or manually
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
cmake -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Linux/Mac

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Your First Actor Application

### Step 1: Include the Framework

```cpp
#include "cas/cas.h"
#include <iostream>
```

The `cas.h` header includes everything you need:
- `cas/actor.h` - Base actor class
- `cas/actor_ref.h` - Actor reference handle
- `cas/message_base.h` - Message base class
- `cas/system.h` - Actor system runtime
- `cas/actor_registry.h` - Name-based actor lookup

### Step 2: Define Your Messages

Messages are simple structs that inherit from `cas::message_base`:

```cpp
struct hello : public cas::message_base {
    std::string name;
};

struct goodbye : public cas::message_base {
    int count;
};
```

### Step 3: Create Your Actor

Actors inherit from `cas::actor` and implement lifecycle hooks:

```cpp
class greeter : public cas::actor {
private:
    int m_greeting_count = 0;

protected:
    // Required: called when system starts
    void on_start() override {
        // Optional: give actor a name for registry lookup
        set_name("greeter");

        // Register message handlers
        handler<hello>(&greeter::on_hello);
        handler<goodbye>(&greeter::on_goodbye);
    }

    // Message handler: must match signature
    void on_hello(const hello& msg) {
        m_greeting_count++;
        std::cout << "Hello, " << msg.name << "! "
                  << "(greeting #" << m_greeting_count << ")"
                  << std::endl;
    }

    void on_goodbye(const goodbye& msg) {
        std::cout << "Goodbye! I said hello "
                  << msg.count << " times." << std::endl;
    }

    // Optional: cleanup before shutdown
    void on_shutdown() override {
        std::cout << "Greeter shutting down..." << std::endl;
    }
};
```

### Step 4: Create and Run

```cpp
int main() {
    // Create actor (returns actor_ref handle)
    auto greeter_ref = cas::system::create<greeter>();

    // Start the actor system
    cas::system::start();

    // Send messages
    hello h1;
    h1.name = "Alice";
    greeter_ref.tell(h1);

    hello h2;
    h2.name = "Bob";
    greeter_ref.tell(h2);

    // Using stream operator
    hello h3;
    h3.name = "Charlie";
    greeter_ref << h3;

    // Send goodbye
    goodbye bye;
    bye.count = 3;
    greeter_ref.tell(bye);

    // Graceful shutdown
    cas::system::shutdown();
    cas::system::wait_for_shutdown();

    return 0;
}
```

Expected output:
```
Hello, Alice! (greeting #1)
Hello, Bob! (greeting #2)
Hello, Charlie! (greeting #3)
Goodbye! I said hello 3 times.
Greeter shutting down...
```

## Complete Example

Here's the complete program:

```cpp
#include "cas/cas.h"
#include <iostream>

// Messages
struct hello : public cas::message_base {
    std::string name;
};

struct goodbye : public cas::message_base {
    int count;
};

// Actor
class greeter : public cas::actor {
private:
    int m_greeting_count = 0;

protected:
    void on_start() override {
        set_name("greeter");
        handler<hello>(&greeter::on_hello);
        handler<goodbye>(&greeter::on_goodbye);
    }

    void on_hello(const hello& msg) {
        m_greeting_count++;
        std::cout << "Hello, " << msg.name << "! "
                  << "(greeting #" << m_greeting_count << ")"
                  << std::endl;
    }

    void on_goodbye(const goodbye& msg) {
        std::cout << "Goodbye! I said hello "
                  << msg.count << " times." << std::endl;
    }

    void on_shutdown() override {
        std::cout << "Greeter shutting down..." << std::endl;
    }
};

// Main
int main() {
    auto greeter_ref = cas::system::create<greeter>();
    cas::system::start();

    hello h;
    h.name = "World";
    greeter_ref.tell(h);

    goodbye bye;
    bye.count = 1;
    greeter_ref.tell(bye);

    cas::system::shutdown();
    cas::system::wait_for_shutdown();
    return 0;
}
```

## Actor-to-Actor Communication

Actors can send messages to each other using the sender information:

```cpp
struct ping : public cas::message_base {
    int id;
};

struct pong : public cas::message_base {
    int id;
};

class pong_actor : public cas::actor {
protected:
    void on_start() override {
        set_name("pong");
        handler<ping>(&pong_actor::on_ping);
    }

    void on_ping(const ping& msg) {
        std::cout << "Pong received ping " << msg.id << std::endl;

        // Reply to sender
        if (msg.sender.is_valid()) {
            pong response;
            response.id = msg.id;
            msg.sender.tell(response);
        }
    }
};

class ping_actor : public cas::actor {
protected:
    void on_start() override {
        set_name("ping");
        handler<pong>(&ping_actor::on_pong);

        // Look up pong actor and send initial ping
        auto pong_ref = cas::actor_registry::get("pong");
        if (pong_ref.is_valid()) {
            ping msg;
            msg.id = 1;
            pong_ref.tell(msg);
        }
    }

    void on_pong(const pong& msg) {
        std::cout << "Ping received pong " << msg.id << std::endl;
    }
};

int main() {
    auto pong_ref = cas::system::create<pong_actor>();
    auto ping_ref = cas::system::create<ping_actor>();

    cas::system::start();

    // Let messages be processed
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    cas::system::shutdown();
    cas::system::wait_for_shutdown();
    return 0;
}
```

## Key Concepts

### Actor References
- `actor_ref` is a lightweight, thread-safe handle
- Can be copied and stored
- Safe to pass between threads
- Check validity with `is_valid()` before using

### Message Handlers
- Register in `on_start()` using `handler<MessageType>()`
- Two styles: member function pointer or lambda
- Handler signature: `void on_msg(const MessageType& msg)`

### System Lifecycle
1. `system::create<T>()` - Create actors (before start)
2. `system::start()` - Start system and call all `on_start()` hooks
3. Send messages - Actors process messages asynchronously
4. `system::shutdown()` - Initiate graceful shutdown
5. `system::wait_for_shutdown()` - Wait for all actors to stop

### Message Delivery
- **Asynchronous** - `tell()` returns immediately
- **FIFO** - Messages processed in send order
- **No blocking** - Sender never waits for receiver

## Common Patterns

### Initialization in on_start()
Always do initialization in `on_start()`, not the constructor:

```cpp
class my_actor : public cas::actor {
private:
    cas::actor_ref m_other_actor;

protected:
    void on_start() override {
        // ✓ GOOD: Registry lookup in on_start
        m_other_actor = cas::actor_registry::get("other");

        // ✓ GOOD: Handler registration
        handler<msg>(&my_actor::on_msg);
    }
};

// ✗ BAD: Don't do this in constructor
// - Registry not populated yet
// - System may not be started
```

### Lambda Handlers
For simple handlers or capturing state:

```cpp
void on_start() override {
    handler<tick>([this](const tick& msg) {
        // Handle tick inline
        m_counter++;
    });
}
```

### Checking Sender Validity
Always check `msg.sender.is_valid()` before using:

```cpp
void on_request(const request& msg) {
    if (msg.sender.is_valid()) {
        response resp;
        resp.data = "result";
        msg.sender.tell(resp);
    } else {
        // Message came from main thread or non-actor
        std::cerr << "No sender to reply to!" << std::endl;
    }
}
```

## Next Steps

- [Creating Actors](30_creating_actors.md) - Advanced actor creation patterns
- [Message Passing](40_message_passing.md) - Message handling in depth
- [Lifecycle Hooks](50_lifecycle_hooks.md) - Master actor lifecycle
- [Actor Registry](60_actor_registry.md) - Name-based lookup
- [Dynamic Removal](70_dynamic_removal.md) - Stop actors at runtime

## Troubleshooting

### Build Errors

**Missing include paths:**
```cmake
target_include_directories(my_app PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)
```

**Link errors:**
```cmake
target_link_libraries(my_app CygnusActorFramework)
```

### Runtime Issues

**Actors not processing messages:**
- Did you call `system::start()`?
- Did you register handlers in `on_start()`?
- Did you wait long enough before shutdown?

**Registry lookup fails:**
- Did you call `set_name()` in `on_start()`?
- Did you lookup after `system::start()`?
- Is the name spelled correctly?

**Crash on shutdown:**
- Did you call both `shutdown()` AND `wait_for_shutdown()`?
- Are you accessing actors after shutdown?
