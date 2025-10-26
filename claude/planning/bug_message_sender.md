# Bug: Message Sender Set to Wrong Actor

## Status
🔴 **CRITICAL BUG** - Blocks all reply-based actor communication

## Discovery Date
2025-10-24

## Summary
The `msg.sender` field is incorrectly set to the **target actor** instead of the **source actor** when sending messages. This causes reply messages to be sent back to the wrong actor.

## Symptoms
- Ping-pong example hangs after first ping
- Pong response is delivered to **pong actor** instead of **ping actor**
- Handler not found errors for response messages
- Worker threads spin indefinitely waiting for messages that never arrive

## Debug Output
```
[PING] sending initial ping to [PONG]
[DISPATCH:pong] Dispatching message, typeid: struct message::ping
[DISPATCH:pong] Handler found, calling...
[PONG] Received ping: 1 - hello from ping
[PONG] Sending pong response to msg.sender...
[DISPATCH:pong] Dispatching message, typeid: struct message::pong  ← WRONG ACTOR!
[DISPATCH:pong] Registered handlers count: 1
[DISPATCH:pong]   - struct message::ping  ← Only has ping handler
[DISPATCH:pong] No handler found!  ← Response delivered to wrong actor!
```

## Root Cause

**File**: `include/cas/actor_ref.h:67-74`

```cpp
template<typename MessageType>
void actor_ref::receive(const MessageType& msg) const {
    if (!actor_ptr_) return;

    // Clone message from stack and enqueue
    auto msg_copy = std::make_unique<MessageType>(msg);
    msg_copy->sender = *this;  // ❌ BUG: *this is the TARGET actor_ref, not SOURCE!
    actor_ptr_->enqueue_message(std::move(msg_copy));
}
```

**Problem**: When `ping_actor` calls `pong_actor_ref.receive(msg)`:
- `*this` refers to `pong_actor_ref` (the target)
- So `msg.sender` is set to `pong_actor_ref`
- When pong replies with `msg.sender.receive(response)`, it sends to itself!

## Expected Behavior
When actor A sends a message to actor B:
```
A → B: message (sender=A)
B → A: response (sent to message.sender, which should be A)
```

## Actual Behavior
When actor A sends a message to actor B:
```
A → B: message (sender=B)  ← BUG: sender is wrongly set to B
B → B: response (sent to message.sender, which is B)  ← Goes back to B!
```

## Proposed Solutions

### Option 1: Pass Sender Explicitly (Breaking Change)
Require sender to be passed to `receive()`:
```cpp
template<typename MessageType>
void actor_ref::receive(const MessageType& msg, const actor_ref& sender) const {
    auto msg_copy = std::make_unique<MessageType>(msg);
    msg_copy->sender = sender;
    actor_ptr_->enqueue_message(std::move(msg_copy));
}

// Usage in actor:
target.receive(msg, self());
```

**Pros**: Explicit and clear
**Cons**: Breaking API change, requires `self()` call everywhere

### Option 2: Set Sender in Actor Context (Recommended)
Have actors automatically set sender when calling `receive()` from within actor methods:
```cpp
// In actor class, wrap actor_ref operations
template<typename MessageType>
void send_to(const actor_ref& target, const MessageType& msg) {
    auto msg_copy = msg;
    msg_copy.sender = self();  // Set sender to self
    target.receive_internal(msg_copy);
}
```

**Pros**: Automatic sender tracking, cleaner API
**Cons**: Requires internal refactoring

### Option 3: Thread-Local Sender Context
Store current actor in thread-local storage:
```cpp
// In system.cpp worker thread:
thread_local actor* current_actor = nullptr;

// In actor_ref:
msg_copy->sender = actor_ref(current_actor->shared_from_this());
```

**Pros**: No API changes, automatic
**Cons**: Requires actors to inherit from `enable_shared_from_this`

## Reproduction Steps
1. Create two actors (ping and pong)
2. Ping sends message to Pong
3. Pong tries to reply using `msg.sender.receive(response)`
4. Response goes to Pong instead of Ping
5. Ping never receives response, system hangs

## Files Affected
- `include/cas/actor_ref.h:72` - Bug location
- `include/cas/message_base.h` - Message sender field
- `tests/example_usage.cpp` - Example that demonstrates the bug

## Test Case
See `tests/example_usage.cpp` - the ping-pong example that currently fails.

## Priority
**CRITICAL** - Core messaging functionality is broken. All request-response patterns fail.

## Next Steps
1. Decide on solution approach (Options 1, 2, or 3)
2. Implement fix
3. Verify ping-pong example works
4. Add unit tests for sender tracking
5. Update API documentation if breaking changes
