# Claude's Notes - Cygnus Actor Framework

## Design Principles

### API Design Philosophy
1. **Super clean API** - Hide all complexity from regular users
2. **Prefer `.` notation over `->` notation** - Use references where possible, provide reference-based alternatives
3. **Behind-the-scenes magic is OK** - Use thread-local storage, singletons, etc. to reduce boilerplate
4. **Zero boilerplate for common cases** - System should auto-initialize, actors should auto-register handlers

### Example of Preferred Style
```cpp
// PREFER THIS (reference-based):
actor_ref ref = get_actor("foo");
ref.receive(msg);  // Using . notation

// OVER THIS (pointer-based):
actor_ref* ref = get_actor("foo");
ref->receive(msg);  // Using -> notation

// OR: Overload to support both
cas::actor_ref ref = ...;
ref.receive(msg);    // . notation
ref << msg;          // Even cleaner
```

### Dependencies Strategy
- **DO NOT reinvent the wheel** for thread-safe collections, lock-free structures, etc.
- **USE well-established open source libraries** with MIT-style licenses (permissive)
- Focus on framework design, not low-level data structure implementation
- Ensure flexibility for future changes (avoid hard coupling to specific libraries)

### Target Libraries to Consider
- **Lock-free queues**: moodycamel::ConcurrentQueue, Boost.Lockfree
- **Thread pools**: Boost.Asio, BS::thread_pool
- **Message serialization** (future): FlatBuffers, Cap'n Proto, msgpack
- All must have MIT, BSD, Boost, or similar permissive licenses

### Namespace
- `cas` - short for Cygnus Actor System

### Naming Convention
- `snake_case` for everything (variables, functions, classes, types)

### Current Implementation Status

#### Completed Headers
- `include/cas/message_base.h` - Base struct for all messages (contains sender)
- `include/cas/actor_ref.h` - Handle/reference to an actor (for sending messages + invoke)
- `include/cas/actor.h` - Base actor class with Acto-style handler registration
- `include/cas/actor_registry.h` - DNS-like lookup for actors by name
- `include/cas/system.h` - Actor system/runtime
- `include/cas/cas.h` - Main header that includes everything

#### Completed Implementations (Basic)
- `source/actor_ref.cpp` - Constructor (templates in header)
- `source/actor.cpp` - Message dispatch, queue management, lifecycle
- `source/actor_registry.cpp` - Singleton registry with thread-safe lookup
- `source/system.cpp` - Thread pool, actor lifecycle, worker threads

#### TODO - Next Steps
- Template implementations for actor_ref (receive, invoke)
- Template implementation for system::create_actor
- Message pooling/reuse
- Lock-free mailbox queues (replace std::queue + mutex)
- Dedicated thread support for certain actors
- Invoke mechanism implementation (RPC calls)
- Condition variables for efficient thread waking (replace sleep)
- Build system and compilation test

### Message Dispatch Design
- Uses `std::type_index` + `std::function` map for type-based dispatch
- Currently requires manual `register_handler<MessageType>()` in `on_start()`
- Goal: Automatic detection and registration of `on_message()` overloads
- See: `planning/automatic_handler_registration.md`

### Threading Model (Planned)
- **Pooled actors**: Run on shared thread pool (default)
- **Dedicated actors**: Own thread for heavy/blocking work
- **Inline actors**: Run on caller's thread (future consideration)

### Message Lifecycle (Planned)
- Messages allocated from pool (avoid malloc in hot path)
- Hybrid API: stack messages auto-cloned, or pre-allocated for zero-copy
- Automatic return to pool after processing

### Actor Communication
- `actor_ref.receive(msg)` - primary API
- `actor_ref.push(msg)` - alternative
- `actor_ref.enqueue(msg)` - alternative
- `actor_ref << msg` - operator overload
- All are thread-safe, all enqueue to actor's mailbox

### Actor Lifecycle
- Actors inherit from `cas::actor`
- Override `on_start()` and `on_stop()` hooks
- Write `on_message(const msg_type& msg)` overloads for each message type
- No required constructors - set name in `on_start()`
- Framework manages actor lifetime

### Actor Discovery
- `actor_registry` - DNS-like name->actor_ref lookup
- `actor_registry::get("actor_name")` returns `actor_ref`
- Actors register themselves via `set_name()` in `on_start()`
- Registry is singleton or thread-local (TBD)

### Future: Inter-Process Communication
- Design with IPC in mind (shared memory, sockets, pipes)
- Messages should be serializable (but not required for local-only)
- Consider hybrid approach: zero-copy local, serialize for remote

## Open Questions
1. Should actor_registry be singleton or thread-local?
2. Should actors auto-start on creation or require explicit `system::start()`?
3. How minimal can we make main() for simple cases?
4. What's the best automatic handler registration approach?

## Research in Progress
- Automatic handler registration using C++17/20 features
- See: `planning/automatic_handler_registration.md`
