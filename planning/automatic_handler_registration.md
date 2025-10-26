# Automatic Message Handler Registration

## Problem Statement

We want users to write message handlers like this:

```cpp
class my_actor : public cas::actor {
protected:
    void on_message(const ping_message& msg) {
        // Handle ping
    }

    void on_message(const pong_message& msg) {
        // Handle pong
    }
};
```

And have the framework automatically detect and register these handlers without requiring manual registration:

```cpp
// We want to AVOID this manual registration:
void on_start() override {
    register_handler<ping_message>();  // Manual - tedious
    register_handler<pong_message>();  // Manual - tedious
}
```

## The Challenge

At runtime, when a message arrives (stored as `message_base*`), we need to:
1. Determine the concrete message type (e.g., `ping_message`)
2. Find the correct `on_message()` overload to call
3. Cast the message and invoke the handler

Currently, we use a `std::unordered_map<std::type_index, std::function>` for dispatch, but we need to populate this map automatically.

## What We Need from C++

### 1. Compile-Time Method Detection (SFINAE/Concepts)

**Detect if a class has a specific method signature:**

```cpp
// Can we detect if MyActor has: void on_message(const PingMessage&) ?
template<typename Actor, typename MessageType>
concept has_message_handler = requires(Actor a, const MessageType& msg) {
    { a.on_message(msg) } -> std::same_as<void>;
};
```

**Status:**
- C++20 Concepts can do this
- C++17 can use SFINAE (more complex)
- C++11/14 can use SFINAE (even more complex)

**Research Topics:**
- C++20 `concepts` and `requires` expressions
- C++17 `std::void_t` and `if constexpr`
- SFINAE with `decltype` and `std::declval`

### 2. Compile-Time Type List Iteration

**We need to check multiple message types:**

```cpp
// Given a list of message types, check which ones Actor handles
template<typename Actor, typename... MessageTypes>
void auto_register_handlers(Actor* actor) {
    // For each MessageType in MessageTypes...
    //   If Actor has on_message(const MessageType&)
    //     Register it
}
```

**Status:**
- C++17 fold expressions make this clean
- C++14 can use recursive templates
- C++11 can use template parameter packs

**Research Topics:**
- Template parameter packs
- Fold expressions (C++17)
- Variadic template recursion
- Type lists (Boost.MPL, std::tuple as type container)

### 3. Global Type Registry (Runtime Discovery)

**Problem:** Even with method detection, we need to know WHICH message types to check for.

**Option A: Compile-time list**
```cpp
// User explicitly lists all message types in the system
using all_message_types = std::tuple<
    ping_message,
    pong_message,
    start_message,
    stop_message
>;

// Framework checks each type against each actor
```

**Option B: Runtime registration**
```cpp
// Each message type registers itself
REGISTER_MESSAGE_TYPE(ping_message);

// Framework maintains global registry of all message types
// At actor creation, iterate through registry and check
```

**Research Topics:**
- Static initialization for global registries
- Template specialization for type registration
- Boost.TypeIndex or similar libraries
- Self-registering types pattern

### 4. Reflection (Future C++ Feature)

**The ideal solution:**

```cpp
// C++ Reflection (proposed for C++26 or later)
for (auto method : reflexpr(MyActor)::get_member_functions()) {
    if (method.name() == "on_message") {
        auto param_type = method.parameters()[0].type();
        register_handler_for_type(param_type);
    }
}
```

**Status:**
- NOT in C++23
- Proposed for future standards (C++26+)
- Some experimental implementations exist

**Research Topics:**
- C++ Reflection proposals (P0194, P1240, etc.)
- Current status of reflection in C++ committee
- Libraries that emulate reflection (RTTR, Ponder, etc.)

### 5. Code Generation / Preprocessing

**Alternative approach: Generate registration code automatically**

**Option A: Custom preprocessor**
- Parse C++ files before compilation
- Generate registration code
- Similar to Qt's MOC (Meta-Object Compiler)

**Option B: Compiler plugins**
- Clang plugin to analyze AST
- Generate registration at compile time

**Research Topics:**
- Qt MOC (Meta-Object Compiler) implementation
- Clang LibTooling
- C++ source code parsing libraries
- Build system integration for code generation

## Practical Approaches (Today's C++)

### Approach 1: Manual Registration (Current)
```cpp
void on_start() override {
    register_handler<ping_message>();
    register_handler<pong_message>();
}
```
**Pros:** Simple, explicit, works with C++11
**Cons:** Boilerplate, easy to forget

### Approach 2: Macro-Assisted Registration
```cpp
#define ACTOR_HANDLES(...)  \
    void _auto_register() { \
        (register_handler<__VA_ARGS__>(), ...); \
    }

class my_actor : public actor {
    ACTOR_HANDLES(ping_message, pong_message)

    void on_message(const ping_message& msg) { }
    void on_message(const pong_message& msg) { }
};
```
**Pros:** Less boilerplate, still explicit
**Cons:** Still requires listing types

### Approach 3: CRTP + Type List
```cpp
template<typename... MessageTypes>
class actor_with_handlers : public actor {
protected:
    void on_start() override {
        (try_register<MessageTypes>(), ...);
        on_start_impl();
    }

    template<typename MsgType>
    void try_register() {
        if constexpr (has_handler<MsgType>()) {
            register_handler<MsgType>();
        }
    }

    virtual void on_start_impl() {}
};

class my_actor : public actor_with_handlers<ping_message, pong_message> {
    void on_message(const ping_message& msg) { }
    void on_message(const pong_message& msg) { }
};
```
**Pros:** Cleaner, compile-time checked
**Cons:** Still need to list message types

### Approach 4: Global Registry + Auto-Detection
```cpp
// In message definitions
struct ping_message : message_base {
    REGISTER_MESSAGE_TYPE(ping_message)
    int id;
};

// Framework automatically checks all registered types
// against each new actor using SFINAE
```
**Pros:** No per-actor boilerplate
**Cons:** Complex implementation, static initialization ordering issues

## Research Questions

1. **Can C++20 concepts simplify the method detection?**
   - How much cleaner is it than C++17 SFINAE?
   - Performance implications?

2. **What libraries exist that solve similar problems?**
   - Boost.Hana for type-level programming
   - Boost.MPL for metaprogramming
   - RTTR, Ponder for reflection
   - Any actor frameworks with automatic dispatch?

3. **How does Qt MOC work internally?**
   - Could we use a similar approach?
   - What are the build system implications?

4. **What's the state of C++ reflection?**
   - Timeline for standardization?
   - Can we use experimental implementations?

5. **Performance considerations:**
   - Compile-time cost of heavy template metaprogramming?
   - Runtime cost of different dispatch mechanisms?
   - Binary size implications?

## Recommended Research Path

1. Start with C++17 `if constexpr` + SFINAE for method detection
2. Look at template parameter packs and fold expressions for iteration
3. Investigate Boost.Hana for type-level programming helpers
4. Research existing actor frameworks (CAF, Theron) dispatch mechanisms
5. Look into Qt MOC as a code generation example
6. Check status of C++ reflection proposals

## Desired Outcome

Ideally, we want:
```cpp
class my_actor : public cas::actor {
    // Just write handlers - framework auto-detects and registers them
    void on_message(const ping_message& msg) { }
    void on_message(const pong_message& msg) { }
    // No manual registration needed!
};
```

The question is: what's the cleanest way to achieve this with modern C++?
