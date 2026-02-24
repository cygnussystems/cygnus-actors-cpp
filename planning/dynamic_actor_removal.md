# Dynamic Actor Removal

## Overview

Add the ability to gracefully stop and remove individual actors at runtime, enabling dynamic market/symbol management in applications like SimTrade.

## Current Limitation

Actors can only be created (`system::create<T>()`) and are destroyed together during system shutdown. There is no way to remove a single actor while the system continues running.

**Current workaround (incomplete):**
```cpp
cas::actor_registry::unregister_actor("orderbook_ES");  // Removes from lookup only
// Actor still exists, processes messages, uses memory
```

## Required API

### Primary Methods

```cpp
// Stop and remove a single actor gracefully
// Returns true if actor was found and stopped, false if not found or already stopped
static bool system::stop_actor(actor_ref ref);

// Stop actor by name (convenience)
static bool system::stop_actor(const std::string& name);

// Check if actor is still running
static bool system::is_actor_running(actor_ref ref);
```

### Actor Reference

```cpp
// Check if actor reference is valid AND actor is running
bool actor_ref::is_running() const;
```

### Optional: Watch Pattern

```cpp
// Watch an actor for termination notification
// When 'watched' actor stops, 'watcher' receives termination_msg
static void system::watch(actor_ref watcher, actor_ref watched);

// Unwatch
static void system::unwatch(actor_ref watcher, actor_ref watched);

// Message sent to watcher when watched actor terminates
struct termination_msg : public cas::message_base {
    std::string actor_name;
    size_t instance_id;
};
```

## Lifecycle During Removal

When `stop_actor()` is called:

1. **Set state** to `actor_state::stopping`
   - New messages are rejected (or queued depending on config)
   
2. **Cancel timers** for this actor
   - Call `cancel_actor_timers(actor*)` (already exists internally)
   
3. **Drain or discard pending messages**
   - Configurable: process remaining messages vs discard immediately
   
4. **Call lifecycle hooks**
   - `on_shutdown()` - actor can save state, notify dependencies
   - `on_stop()` - final cleanup
   
5. **Remove from thread's actor list**
   - Thread-safe removal from `m_actors_per_thread[thread_id]`
   
6. **Unregister from registry**
   - Call `actor_registry::unregister_actor(name)`
   
7. **Release ownership**
   - Remove `shared_ptr` from system, allowing actor destruction

## Implementation Notes

### Thread Safety

The main challenge is removing an actor from `m_actors_per_thread[thread_id]` while:
- The worker thread is iterating over its actor list
- Other threads may be sending messages to the actor

**Suggested approach:**

1. Set actor state to `stopping` (atomic, immediate visibility)
2. Worker thread checks state before processing - skips `stopping` actors
3. Use mutex for actual vector removal, or:
4. Mark as "pending removal" and batch-remove at safe point

### Message Queue Handling

Options for pending messages when stopping:

| Mode | Behavior |
|------|----------|
| `drain` | Process remaining messages before stop |
| `discard` | Drop pending messages immediately |
| `reject` | Return messages to sender (if applicable) |

Suggested default: `drain` for graceful shutdown

### Actor Dependencies

If actor A holds a reference to actor B, and B is stopped:
- Messages sent to B will be silently ignored (actor_ref becomes invalid)
- Actor A may need notification (watch pattern)

## Configuration

```cpp
struct stop_config {
    std::chrono::milliseconds drain_timeout{1000};  // Max time to process remaining messages
    bool wait_for_drain{true};                       // Block until drained or timeout
    bool notify_watchers{true};                      // Send termination_msg to watchers
};
```

## Usage Example (SimTrade)

```cpp
// Add market dynamically
void MarketActorManager::add_market(const Instrument& inst) {
    auto ref = cas::system::create<OrderBookActor>(inst.symbol);
    orderbook_actors_[inst.symbol] = ref;
}

// Remove market dynamically
void MarketActorManager::remove_market(const std::string& symbol) {
    auto ref = orderbook_actors_.at(symbol);
    
    // Optional: watch for termination confirmation
    auto self = cas::actor_ref::self();  // If called from actor
    cas::system::watch(self, ref);
    
    // Stop the actor
    cas::system::stop_actor(ref);
    
    orderbook_actors_.erase(symbol);
}

// In OrderBookActor
void OrderBookActor::on_shutdown() {
    // Save state to database
    save_order_book_snapshot();
    
    // Notify account actor of pending orders cancellation
    auto account = cas::actor_registry::get("account");
    if (account.is_running()) {
        account.receive(market_closing_msg{symbol_});
    }
}
```

## Testing Requirements

1. **Basic removal**: Create actor, stop it, verify removed from registry
2. **Pending messages**: Stop actor with pending messages, verify drain/discard
3. **Active timers**: Stop actor with scheduled timers, verify cancelled
4. **Message to stopped actor**: Send to stopped actor, verify no-op
5. **Multiple removals**: Create N actors, remove subset, verify remaining work
6. **Removal during processing**: Stop actor while processing message
7. **Watch pattern**: Verify watcher receives termination notification
8. **Thread safety**: Stop actors from different threads simultaneously

## Impact on Existing Code

- Minimal changes to existing API
- `system::shutdown()` can use same internal removal logic for each actor
- No changes to message passing or handler registration

## Priority

**High** - Required for SimTrade brokerage simulation where markets can be added/removed dynamically.
