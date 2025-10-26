# Ask Pattern Design - Request/Response (RPC-style)

## Goal
Support synchronous request-response pattern that feels like a function call:
```cpp
int result = actor_ref.ask<int>(calculate_request{42});
```

## Design Decision: Two-Queue System

Each actor maintains:
1. **Regular mailbox** - fire-and-forget messages (receive/push/enqueue)
2. **Ask queue** - priority request-response messages

### Processing Priority
```
while (actor is running):
    1. Check ask queue first - process if available
    2. Then check regular mailbox - process if available
    3. Wait for new messages
```

### Why Two Queues?

**Function-call semantics require priority:**
- ask() should not wait behind 1000 queued regular messages
- Caller is blocked waiting for response
- Should process "soon" after current message finishes

**Alternatives considered:**
- Same queue: Unpredictable latency, not RPC-like
- Insert at front: Works but messy with single queue
- Inline execution: Breaks actor model, reentrancy issues

**Two queues gives:**
- Clean separation of concerns
- Natural priority for ask()
- Simple implementation
- Predictable behavior

## API

### Simple blocking ask (no timeout)
```cpp
response_type resp = actor_ref.ask<response_type>(request_msg);
```

**Behavior:**
- Blocks until response received
- Could hang forever if target actor crashes/deadlocks
- Use for trusted actors or short operations

### Ask with timeout (returns optional)
```cpp
auto resp = actor_ref.ask<response_type>(request_msg, 500ms);
if (resp) {
    // Got response: *resp
} else {
    // Timeout
}
```

**Behavior:**
- Waits up to timeout
- Returns `std::optional<response_type>`
- Safe for untrusted actors or slow operations

## Responding to ask()

Actor handles ask messages like regular messages, but uses `reply()` to respond:

```cpp
void on_message(const calculate_request& msg) {
    int result = expensive_calculation(msg.value);
    msg.sender.reply(result);  // Unblocks caller
}
```

**Note:** `reply()` works for ask(), regular `receive()` works for fire-and-forget

## Implementation Details

### ask() Implementation Sketch
```cpp
template<typename ResponseType, typename MessageType>
ResponseType actor_ref::ask(const MessageType& msg) {
    // Create promise/future for response
    auto promise = std::make_shared<std::promise<ResponseType>>();
    auto future = promise->get_future();

    // Create temporary response handler
    auto temp_actor = create_temp_response_handler([promise](const ResponseType& resp) {
        promise->set_value(resp);
    });

    // Set sender to temp actor
    MessageType msg_copy = msg;
    msg_copy.sender = temp_actor;

    // Enqueue to ASK QUEUE (not regular mailbox)
    actor_ptr_->enqueue_ask_message(msg_copy);

    // Block waiting for response
    return future.get();
}
```

### ask() with timeout
```cpp
template<typename ResponseType, typename MessageType>
std::optional<ResponseType> actor_ref::ask(const MessageType& msg,
                                           std::chrono::milliseconds timeout) {
    // Same setup as above...

    // Wait with timeout
    if (future.wait_for(timeout) == std::future_status::ready) {
        return future.get();
    } else {
        // Timeout - cleanup temp actor
        return std::nullopt;
    }
}
```

### reply() Implementation
```cpp
template<typename ResponseType>
void actor_ref::reply(const ResponseType& response) {
    // Send response to sender (temp actor waiting)
    this->receive(response);
}
```

## Thread Safety

Both queues must be thread-safe:
- **Regular mailbox**: MPSC (multiple producer, single consumer)
- **Ask queue**: MPSC (multiple producer, single consumer)

Use lock-free queues (moodycamel or similar) for both.

## Edge Cases

### Self-ask (deadlock detection)
```cpp
// Actor asks itself - DEADLOCK!
auto result = self().ask<int>(my_request);  // Would block forever
```

**Solution:** Detect and either:
- Throw exception
- Process inline (allow reentrancy)
- Document as "don't do this"

### Multiple concurrent ask() calls
Multiple threads can ask() the same actor simultaneously:
- All requests go to ask queue
- Processed one at a time (FIFO within ask queue)
- Each caller blocks until their response arrives

### Actor crashes during ask()
If target actor crashes while processing ask():
- Caller blocks forever (without timeout)
- With timeout: returns std::nullopt

**Future:** Could detect actor death and throw exception

### Response type mismatch
```cpp
// Caller expects int, actor replies with string
auto result = actor_ref.ask<int>(msg);  // Runtime error!
```

**Solution:** Type erasure + runtime check, throw exception if mismatch

## Notes

- ask() is for **request-response** only
- Use regular receive() for **fire-and-forget**
- ask() should be used sparingly (actor model prefers async)
- Useful for: queries, calculations, synchronization points

## Future Enhancements

1. **Typed ask channels** - Compile-time guarantee of response type
2. **Async ask** - Returns std::future instead of blocking
3. **Batch ask** - Send multiple requests, wait for all responses
4. **Ask with callback** - Non-blocking alternative
