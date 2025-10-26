# Lock-Free Queue Integration - Remaining Work

## Status
- ✅ Downloaded moodycamel/concurrentqueue.h (149KB)
- ✅ Added to include/external/
- ✅ Updated actor.h includes and queue declarations
- ✅ Updated enqueue_message() in actor.cpp (removed mutex, using .enqueue())
- ❌ **NEED TO UPDATE**: enqueue_ask_message() in actor.cpp
- ❌ **NEED TO UPDATE**: process_next_message() in actor.cpp
- ❌ **NEED TO UPDATE**: has_messages() in actor.cpp
- ❌ **NEED TO UPDATE**: queue_size() in actor.cpp
- ❌ **BUILD AND TEST**

## Remaining Changes in actor.cpp

### 1. enqueue_ask_message() (lines 150-196)
Replace:
```cpp
{
    std::lock_guard<std::mutex> lock(m_ask_queue_mutex);
    m_ask_queue.push(std::move(msg));
    ask_size = m_ask_queue.size();
}
// ... later ...
{
    std::lock_guard<std::mutex> lock(m_mailbox_mutex);
    mailbox_size = m_mailbox.size();
}
```

With:
```cpp
m_ask_queue.enqueue(std::move(msg));
ask_size = m_ask_queue.size_approx();
mailbox_size = m_mailbox.size_approx();
```

### 2. process_next_message() (around lines 240-270)
Replace:
```cpp
{
    std::lock_guard<std::mutex> lock(m_ask_queue_mutex);
    if (!m_ask_queue.empty()) {
        msg = std::move(m_ask_queue.front());
        m_ask_queue.pop();
        from_ask = true;
    }
}
// ... later ...
{
    std::lock_guard<std::mutex> lock(m_mailbox_mutex);
    if (!m_mailbox.empty()) {
        msg = std::move(m_mailbox.front());
        m_mailbox.pop();
    }
}
```

With:
```cpp
// Try ask queue first (priority)
if (m_ask_queue.try_dequeue(msg)) {
    from_ask = true;
}
// If no ask message, try regular mailbox
else if (!m_mailbox.try_dequeue(msg)) {
    return;  // No messages
}
```

### 3. has_messages() (around lines 275-290)
Replace:
```cpp
{
    std::lock_guard<std::mutex> lock(m_ask_queue_mutex);
    if (!m_ask_queue.empty()) {
        return true;
    }
}
{
    std::lock_guard<std::mutex> lock(m_mailbox_mutex);
    return !m_mailbox.empty();
}
```

With:
```cpp
// Check ask queue first
std::unique_ptr<message_base> msg;
if (m_ask_queue.try_dequeue(msg)) {
    // Put it back
    m_ask_queue.enqueue(std::move(msg));
    return true;
}
// Check mailbox
if (m_mailbox.try_dequeue(msg)) {
    // Put it back
    m_mailbox.enqueue(std::move(msg));
    return true;
}
return false;
```

**ALTERNATIVE** (better performance, less accurate):
```cpp
// Approximate check - may have false positives/negatives but fast
return m_ask_queue.size_approx() > 0 || m_mailbox.size_approx() > 0;
```

### 4. queue_size() (around lines 295-305)
Replace:
```cpp
{
    std::lock_guard<std::mutex> lock(m_ask_queue_mutex);
    total += m_ask_queue.size();
}
{
    std::lock_guard<std::mutex> lock(m_mailbox_mutex);
    total += m_mailbox.size();
}
```

With:
```cpp
total += m_ask_queue.size_approx();
total += m_mailbox.size_approx();
```

## moodycamel API Reference

### Enqueue (Producer)
```cpp
queue.enqueue(std::move(item));           // Always succeeds (may allocate)
queue.try_enqueue(std::move(item));       // Returns false if would allocate
```

### Dequeue (Consumer)
```cpp
T item;
bool got = queue.try_dequeue(item);       // Returns false if empty
```

### Size
```cpp
size_t approx = queue.size_approx();      // Approximate size, very fast
```

### Notes
- No `.empty()`, `.front()`, `.pop()`, `.size()` methods
- `.size_approx()` is O(1) but approximate
- `.try_dequeue()` is the only way to check if empty AND get element
- All operations are lock-free

## Testing Plan
1. Build project
2. Run all 66 tests
3. Check for:
   - Compilation errors
   - Runtime failures
   - Performance improvements (should be faster, less contention)
   - Memory usage (should be similar or better, dynamic allocation)

## Expected Benefits
- **No lock contention** on message sends
- **Lower latency** for high-frequency messaging
- **Better scalability** with many actors
- **Dynamic sizing** - no upfront capacity planning needed
