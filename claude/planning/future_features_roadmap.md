# Cygnus Actor Framework - Future Features Roadmap

**Date**: 2025-10-25

This document outlines missing features compared to mature actor frameworks (Akka, CAF, Actix) and proposes a development roadmap.

## Current Status

### Implemented Features ✅
- Actor lifecycle (on_start, on_shutdown, on_stop)
- Message passing with type-safe handlers
- Thread affinity (actors pinned to threads)
- Two-priority queues (regular + ask)
- Actor registry (name-based lookup)
- Graceful shutdown with monitoring and logging
- Fast actors (dedicated threads with configurable polling)
- Stateful actors (state machine pattern)

## Missing Features Analysis

### 1. Supervision & Fault Tolerance 🔴 CRITICAL

**Priority**: MUST HAVE before 1.0

**Problem**: One actor crashes → takes down entire system or silently fails

**What mature frameworks provide**:
- Supervision hierarchies (parent monitors children)
- Restart strategies (one-for-one, all-for-one, escalate)
- Error isolation (actor failures don't propagate)
- Configurable supervision policies

**Proposed API**:
```cpp
class supervisor : public cas::actor {
protected:
    supervision_strategy strategy() override {
        return one_for_one()
            .max_retries(3)
            .within(std::chrono::seconds(10))
            .on_failure([](actor_ref child, std::exception& error) {
                // Restart, stop, or escalate
            });
    }

    void on_start() override {
        // Supervised children
        auto child = spawn<worker_actor>();
    }
};
```

**Design considerations**:
- Parent-child relationship tracking
- Exception handling in message dispatch
- State preservation/reset on restart
- Death notifications
- Supervision tree visualization (for debugging)

---

### 2. Ask Pattern (Request-Response) 🟡 HIGH PRIORITY

**Priority**: Important for real use

**Problem**: Manual reply tracking with correlation IDs is painful

**Current status**: Infrastructure exists (ask queue, invoke_handler registration) but user-facing API is not implemented

**What's missing**:
```cpp
// Synchronous ask
auto future = actor_ref.ask<response>(request_msg, 5s);
auto result = future.get();  // Block until response or timeout

// Async ask
actor_ref.ask<response>(request_msg).then([](response r) {
    // Handle response
}).on_timeout([]() {
    // Handle timeout
});

// Or the invoke() API (stubs exist but not implemented):
auto result = actor_ref.invoke<double>(calculate_op{}, 42, 5);
```

**What we have** ✅:
- Ask queue (priority queue processed before regular mailbox)
- `enqueue_ask_message()` method
- `invoke_handler<>()` registration template

**What's needed** ❌:
- Implement `actor_ref.invoke<ReturnType>(op, args...)` (currently TODO stubs)
- Future/promise mechanism for blocking/waiting
- Correlation ID tracking (match request to response)
- Timeout handling

**Design considerations**:
- Future/promise implementation (std::future or custom)
- Timeout handling
- Correlation ID management (automatic, hidden from user)
- Error propagation when target actor is stopped

**Current workaround**: Users manually track correlation IDs

---

### 3. Actor Addresses & Remote Messaging 🟡 HIGH for distributed systems

**Priority**: Separate milestone (post-1.0)

**What mature frameworks provide**:
- Location transparency: actor_ref works for local AND remote actors
- Serialization: Messages sent over network
- Actor addresses: `cas://system@host:port/user/actor_name`
- Cluster membership
- Failure detection across nodes

**Proposed API**:
```cpp
// Remote actor reference
auto remote = system::actor_for("cas://192.168.1.5:8080/worker");
remote.receive(msg);  // Transparent network send

// Local/remote transparent
actor_ref ref = /* could be local or remote */;
ref.receive(msg);  // Works either way
```

**Design considerations**:
- Network protocol (TCP, UDP, custom)
- Message serialization (Protobuf, MessagePack, custom)
- Connection management
- Routing tables
- Network partition handling
- Security (authentication, encryption)

**Impact**: Massive undertaking, requires serialization framework

---

### 4. Backpressure & Flow Control 🟠 MEDIUM PRIORITY

**Priority**: Important for production stability

**Problem**: Fast sender overwhelms slow receiver → memory exhaustion

**What's missing**:
- Bounded mailboxes (reject/block when full)
- Backpressure signals (slow down sender)
- Stream processing (reactive streams style)
- Flow control strategies

**Proposed API**:
```cpp
class bounded_actor : public cas::actor {
protected:
    mailbox_config mailbox() override {
        return mailbox_config()
            .max_size(1000)
            .on_overflow(overflow_strategy::drop_oldest);
            // Options: drop_oldest, drop_newest, reject, block
    }
};
```

**Design considerations**:
- Overflow strategies (drop, reject, block sender)
- Metrics (mailbox depth, dropped messages)
- Integration with ask pattern (reject asks when full)
- Performance impact of bounded queues

**Current state**: Unbounded queues only

---

### 5. Timers & Scheduled Messages 🟠 MEDIUM PRIORITY

**Priority**: Important for real use (very common pattern)

**What's missing**:
```cpp
class timer_actor : public cas::actor {
protected:
    void on_start() override {
        schedule_once(5s, msg::timeout{});
        schedule_periodic(100ms, msg::heartbeat{});

        auto timer_id = schedule_once(10s, msg::delayed{});
        cancel_timer(timer_id);  // Cancellation
    }
};
```

**Design considerations**:
- Timer wheel or priority queue implementation
- Timer thread vs piggyback on worker threads
- Timer cancellation
- Accuracy guarantees (soft real-time)
- Integration with actor lifecycle (cancel on stop)

**Current workaround**: External timer threads + manual message sending

---

### 6. Actor Lifecycle Events 🟢 NICE TO HAVE

**Priority**: Polish

**What's missing**:
- `on_pre_restart()` / `on_post_restart()` hooks
- Death watch: Monitor another actor, get notified when it stops
- `become/unbecome` (runtime behavior switching)
- Actor context queries (children, parent, sender)

**Proposed API**:
```cpp
class lifecycle_actor : public cas::actor {
protected:
    void on_start() override {
        auto child = spawn<worker>();
        watch(child);  // Death watch
    }

    void on_actor_terminated(actor_ref who) override {
        // Notified when watched actor stops
    }

    void on_pre_restart(std::exception& reason) override {
        // Save state before restart
    }

    void on_post_restart() override {
        // Restore state after restart
    }

    void handle_state_a(const msg::input& m) {
        // Process in state A
        become(&lifecycle_actor::handle_state_b);  // Switch behavior
    }

    void handle_state_b(const msg::input& m) {
        // Process in state B
        unbecome();  // Return to previous behavior
    }
};
```

**Design considerations**:
- Death watch registry (who watches whom)
- Behavior stack for become/unbecome
- Integration with supervision

---

### 7. Message Patterns 🟢 NICE TO HAVE

**Priority**: Developer ergonomics

**Common patterns not supported**:

**Forward**: Pass message to another actor, preserve original sender
```cpp
void on_message(const msg::request& m) {
    other_actor.forward(m);  // Sender stays original, not this actor
}
```

**Pipe**: Chain async operations
```cpp
ask(db_actor, query).pipe_to(http_actor);  // Result flows through
```

**Broadcast**: Send to multiple actors
```cpp
actor_group workers = {worker1, worker2, worker3};
workers.broadcast(msg::shutdown{});
```

**Router patterns**:
- Round-robin: Distribute messages evenly
- Random: Random selection
- Smallest mailbox: Send to least busy
- Consistent hash: Route by message key

**Proposed API**:
```cpp
// Router example
auto router = system::create_router<round_robin_router>();
router.add_routee(worker1);
router.add_routee(worker2);
router.add_routee(worker3);

router.receive(msg);  // Automatically routed
```

---

### 8. Metrics & Observability 🟢 NICE TO HAVE

**Priority**: Operational visibility (production monitoring)

**What's needed**:
- Mailbox depth metrics (current size, max size)
- Message processing latency (histogram)
- Actor creation/death counts
- Message throughput (messages/sec)
- Deadlock detection (circular dependencies)
- System health dashboard

**Proposed API**:
```cpp
// Query metrics
auto metrics = system::metrics();
std::cout << "Total actors: " << metrics.actor_count() << "\n";
std::cout << "Messages/sec: " << metrics.throughput() << "\n";
std::cout << "Avg latency: " << metrics.avg_latency() << "ms\n";

// Per-actor metrics
auto actor_metrics = actor_ref.metrics();
std::cout << "Mailbox depth: " << actor_metrics.mailbox_depth() << "\n";
```

**Design considerations**:
- Low overhead (metrics shouldn't slow down system)
- Sampling vs full instrumentation
- Export formats (Prometheus, JSON)
- Real-time vs historical
- Grafana/Prometheus integration

---

### 9. Testing Support 🟢 NICE TO HAVE

**Priority**: Developer ergonomics

**What testing frameworks provide**:
```cpp
TEST_CASE("actor responds to ping") {
    TestProbe probe;
    auto system = TestActorSystem::create();
    auto actor = system.spawn<ping_actor>();

    // Send message
    actor.receive(msg::ping{});

    // Assert response within timeout
    auto response = probe.expect_message<msg::pong>(500ms);
    REQUIRE(response.value == 42);

    // Assert no more messages
    probe.expect_no_message(100ms);
}
```

**Design considerations**:
- Test actor system (isolated, deterministic)
- Test probes (message capture and assertions)
- Time control (virtual time for testing)
- Deterministic scheduling (for reproducibility)
- Integration with Catch2

---

## Development Roadmap

### Phase 1: Core Stability (Pre-1.0)
**Target**: Production-ready single-process system

1. **Supervision & Fault Tolerance** (4-6 weeks)
   - Parent-child relationships
   - Restart strategies (one-for-one, all-for-one)
   - Exception handling in dispatch
   - Death notifications

2. **Ask Pattern** (2-3 weeks)
   - Future/promise implementation
   - Timeout handling
   - Automatic correlation ID management

3. **Bounded Mailboxes** (1-2 weeks)
   - Overflow strategies
   - Backpressure signaling
   - Metrics integration

4. **Timers** (2-3 weeks)
   - Timer wheel implementation
   - Schedule once/periodic
   - Cancellation support

**Milestone**: v1.0 Release
- Production-ready for single-process applications
- Comprehensive test suite
- Documentation and examples

---

### Phase 2: Advanced Features (Post-1.0)
**Target**: Enhanced developer experience

5. **Extended Lifecycle Events** (1-2 weeks)
   - Death watch
   - Pre/post restart hooks
   - Become/unbecome

6. **Message Patterns** (2-3 weeks)
   - Forward with sender preservation
   - Routers (round-robin, random, etc.)
   - Broadcast groups

7. **Metrics & Observability** (2-3 weeks)
   - Core metrics collection
   - Prometheus export
   - Health checks

8. **Testing Framework** (2-3 weeks)
   - Test probes
   - Virtual time
   - Deterministic scheduling

**Milestone**: v1.5 Release
- Enhanced developer ergonomics
- Production monitoring support

---

### Phase 3: Distribution (Future)
**Target**: Multi-node clusters

9. **Serialization Framework** (4-6 weeks)
   - Message serialization API
   - Schema evolution
   - Integration with Protobuf/MessagePack

10. **Remote Actors** (8-12 weeks)
    - Network protocol
    - Location transparency
    - Remote actor references
    - Connection management

11. **Cluster Support** (6-8 weeks)
    - Membership (gossip protocol)
    - Failure detection
    - Cluster sharding
    - Distributed registry

**Milestone**: v2.0 Release
- Full distributed actor system
- Multi-node clusters
- Network transparency

---

## Design Principles

### Non-Goals
- GUI/web frameworks (actors only)
- Database integration (user responsibility)
- Specific serialization format (pluggable)

### Constraints
- Zero external dependencies for core (header-only where possible)
- C++17 minimum (no C++20 requirements)
- Cross-platform (Windows, Linux, macOS)
- Performance competitive with CAF/Actix

### Trade-offs
- Ease of use vs performance (favor ease of use)
- Type safety vs flexibility (favor type safety)
- Feature completeness vs simplicity (favor simplicity for 1.0)

---

## Next Steps

**Immediate priorities** (next session):
1. Design supervision API and hierarchy tracking
2. Implement basic supervisor with one-for-one strategy
3. Add exception handling to message dispatch
4. Write tests for actor restart scenarios

**Documentation needed**:
- Architecture decision records (ADRs) for major features
- API design docs for each phase
- Migration guides between versions

**Community/ecosystem** (future):
- Examples repository (common patterns)
- Benchmarks vs other frameworks
- Integration guides (web servers, databases, etc.)

---

## Open Questions

1. **Supervision**: Should we support dynamic supervision trees, or only static hierarchies?
2. **Ask pattern**: std::future, custom future, or callback-based?
3. **Remote/IPC**: ✅ **DECISION: Use ZeroMQ** - Don't reinvent the wheel. ZeroMQ provides battle-tested messaging patterns (REQ/REP, PUB/SUB, DEALER/ROUTER) that map naturally to actor patterns. Mature, cross-platform, excellent performance.
4. **Serialization**: Mandatory or optional? Code generation or reflection?
5. **Memory model**: Shared-nothing strict, or allow shared state with locks?

---

## Comparison Matrix

| Feature | Akka | CAF | Actix | Cygnus (current) | Cygnus (1.0 goal) |
|---------|------|-----|-------|------------------|-------------------|
| Basic actors | ✅ | ✅ | ✅ | ✅ | ✅ |
| Type-safe messages | ✅ | ✅ | ✅ | ✅ | ✅ |
| Supervision | ✅ | ✅ | ✅ | ❌ | ✅ |
| Ask pattern | ✅ | ✅ | ✅ | ❌ | ✅ |
| Timers | ✅ | ✅ | ✅ | ❌ | ✅ |
| Backpressure | ✅ | ✅ | ✅ | ❌ | ✅ |
| Remote actors | ✅ | ✅ | ❌ | ❌ | v2.0 |
| Cluster | ✅ | ✅ | ❌ | ❌ | v2.0 |
| Streams | ✅ | ✅ | ✅ | ❌ | Future |
| HTTP integration | ✅ | ❌ | ✅ | ❌ | Future |

---

**Last updated**: 2025-10-25
