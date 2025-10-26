# Ask Pattern Implementation Status

**Date**: 2025-10-25

## Summary

We started implementing the ask pattern (RPC-style request-response) but encountered circular dependency issues. The implementation is ~80% complete but doesn't compile yet.

## What's Done ✅

1. **Message ID tracking** - Complete and working
   - Added `message_id` and `correlation_id` to `message_base`
   - Atomic counter in `system` assigns unique IDs
   - All messages automatically get IDs when sent

2. **API renamed** - `invoke()` → `ask()`
   - `actor_ref.ask<ReturnType>(op_tag, args...)` - blocking
   - `actor_ref.ask<ReturnType>(op_tag, timeout, args...)` - with timeout
   - Handler registration: `ask_handler<ReturnType, OpTag>(&Actor::method)`

3. **Core infrastructure**
   - Ask queue exists (priority queue)
   - Handler registration map (`m_ask_handlers`)
   - Template signatures defined

## What's Not Done ❌

1. **Circular dependency issues**
   - `ask_message.h` needs full definition of `actor` and `message_base`
   - But actor.h and message_base.h can't include ask_message.h without circular includes
   - Need to reorganize headers or move implementations to .cpp files

2. **ask_request implementation**
   - Created `ask_request<ReturnType, OpTag, Args...>` struct
   - Has promise/future mechanism
   - Has `process()` method to call handler
   - But circular deps prevent compilation

3. **Dispatch integration**
   - Added check in `actor::dispatch_message()` for `ask_request_base`
   - Calls `ask_msg->process(this)`
   - But can't compile due to circular deps

## Files Modified (Not Yet Compiling)

- `include/cas/message_base.h` - Added message_id, correlation_id
- `include/cas/system.h` - Added m_next_message_id counter
- `source/system.cpp` - Implemented next_message_id()
- `include/cas/actor_ref.h` - Renamed invoke→ask, implemented send logic
- `include/cas/actor.h` - Renamed m_invoke_handlers→m_ask_handlers, renamed invoke_handler→ask_handler
- `include/cas/ask_message.h` - NEW FILE (circular dependency issues)
- `source/actor.cpp` - Added ask_request dispatch logic

## Solution Approaches

### Option 1: Move template implementations to .cpp (Recommended)
Move `ask_request::process()` implementation from header to a new `ask_message.cpp` file. This breaks the circular dependency chain.

**Pros**: Clean separation, standard C++ pattern
**Cons**: Requires explicit template instantiation for each OpTag/Args combo used

### Option 2: Reorganize headers
Create an internal header `internal/ask_impl.h` that's included only where needed (actor.cpp, actor_ref templates). Keep user-facing headers clean.

**Pros**: Templates stay in headers
**Cons**: More complex include structure

### Option 3: Use type erasure
Instead of `ask_request<ReturnType, OpTag, Args...>`, use `std::function` and `void*` for args/results.

**Pros**: Simplifies templates
**Cons**: Loses type safety, more runtime overhead

## Recommended Next Steps

1. **Create `source/ask_message.cpp`**
2. **Move `ask_request::process()` implementation there**
3. **Add explicit template instantiations** for common types
4. **Or**: Use Option 2 with internal headers

## User API (When Complete)

```cpp
// Define operation tags (empty structs)
struct calculate_profit_op {};

// Actor registers handler
class my_actor : public cas::actor {
protected:
    void on_start() override {
        set_name("calc");
        ask_handler<double, calculate_profit_op>(&my_actor::calc_profit);
    }

    double calc_profit(int sales, int costs) {
        return sales - costs;
    }
};

// Another actor calls it
void some_handler() {
    auto calc = cas::actor_registry::lookup("calc");

    // Blocking call
    double profit = calc.ask<double>(calculate_profit_op{}, 1000, 600);
    // profit == 400.0

    // With timeout
    auto opt = calc.ask<double>(calculate_profit_op{}, 5s, 1000, 600);
    if (opt) {
        double profit = *opt;
    } else {
        // Timeout
    }
}
```

## Testing Plan (When Working)

1. Simple ask/response with int
2. Ask with timeout (success case)
3. Ask with timeout (timeout case)
4. Ask with no handler registered (should get exception in promise)
5. Ask to stopped actor
6. Concurrent asks to same actor
7. Ask from non-actor context (main)

## Current Build Status

❌ **Does not compile** - circular dependency errors

Last error:
```
actor.cpp(97): error C2680: 'cas::ask_request_base *': invalid target type for dynamic_cast
'cas::ask_request_base': class must be defined before using in a dynamic_cast
```

## Files to Review/Fix Next Session

- `include/cas/ask_message.h` - Fix circular deps
- `source/actor.cpp` - Ensure dispatch works
- Create test case for basic ask pattern

---

**Status**: BLOCKED on circular dependencies
**Next Action**: Choose solution approach and implement
