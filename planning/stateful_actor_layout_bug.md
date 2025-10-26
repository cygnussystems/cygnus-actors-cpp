# Stateful Actor Memory Layout Bug

## Problem Summary

Adding a simple `size_t m_instance_id = 0;` member variable to the base `actor` class causes `stateful_actor` tests to crash with segmentation fault, while all other actor types work perfectly.

## Test Results

### Baseline (without m_instance_id)
- **Status**: ✅ All tests pass
- **Result**: `All tests passed (107 assertions in 42 test cases)`

### With m_instance_id Added
- **Regular actors**: ✅ All tests pass (40/42)
- **Fast actors**: ✅ All tests pass
- **Inline actors**: ✅ All tests pass
- **Stateful actors**: ❌ Crash with SIGSEGV (2/42 tests fail)

### Test Filtering Results
```bash
# Excluding stateful actor tests
"cmake-build-debug\unit_tests.exe" "~[stateful]"
Result: All tests passed (101 assertions in 40 test cases)

# Only our minimal instance_id test
"cmake-build-debug\unit_tests.exe" "[instance_id]"
Result: All tests passed (1 assertion in 1 test case)
```

## Crash Details

### Error Message
```
-------------------------------------------------------------------------------
Stateful actor rejects messages in wrong state
-------------------------------------------------------------------------------
C:\Users\ritte\_PR_DEV_\DEV_CPP\CYGNUS_ACTOR_FRAMEWORK\tests\04_advanced\test_stateful_actor.cpp(75)

C:\Users\ritte\_PR_DEV_\DEV_CPP\CYGNUS_ACTOR_FRAMEWORK\tests\04_advanced\test_stateful_actor.cpp(75): FAILED:
  {Unknown expression after the reported line}
due to a fatal error condition:
  SIGSEGV - Segmentation violation signal

===============================================================================
test cases: 33 | 32 passed | 1 failed
assertions: 88 | 87 passed | 1 failed
```

### Crash Location
- **Test file**: `tests/04_advanced/test_stateful_actor.cpp`
- **Line reported**: 75 (TEST_CASE declaration)
- **Actual crash**: During test execution, likely at `cas::system::start()` on line 80
- **Note**: 32 tests completed before crash, so NOT a static initialization issue

## Code Changes Made

### Change 1: Added member to actor.h
```cpp
// In include/cas/actor.h, private section
// Tried multiple positions:

// Position 1: After m_self_ref (before m_assigned_thread_id)
size_t m_instance_id = 0;  // ❌ Crashes stateful_actor

// Position 2: After m_state atomic (before m_mailbox)
size_t m_instance_id = 0;  // ❌ Crashes stateful_actor

// Position 3: At end of private section (after m_ask_handlers)
size_t m_instance_id = 0;  // ❌ Still crashes stateful_actor
```

**Conclusion**: Position doesn't matter - any addition to base class breaks stateful_actor

### Change 2: Added getter method
```cpp
// In include/cas/actor.h, public section
size_t instance_id() const;

// In source/actor.cpp
size_t actor::instance_id() const {
    return m_instance_id;
}
```

### Change 3: Disabled move operations
```cpp
// In include/cas/actor.h, public section
// Non-copyable, non-movable
actor(const actor&) = delete;
actor& operator=(const actor&) = delete;
actor(actor&&) = delete;              // ← Added
actor& operator=(actor&&) = delete;   // ← Added
```

### Change 4: Added minimal test
```cpp
// In tests/00_basic/test_actor_creation.cpp
TEST_CASE("Minimal test - instance_id member", "[00_basic][instance_id]") {
    class minimal_actor : public cas::actor {
    protected:
        void on_start() override {
            size_t id = instance_id();
            (void)id;
        }
    };

    auto actor = cas::system::create<minimal_actor>();
    REQUIRE(actor.is_valid());

    cas::system::start();
    wait_ms(10);

    cas::system::shutdown();
    cas::system::wait_for_shutdown();

    TEST_CLEANUP();
}
```
**Result**: ✅ This test passes!

## Actor Class Hierarchy

```
actor (base class)
├── fast_actor (dedicated thread, polling) ✅ Works
├── inline_actor (no lifecycle hooks) ✅ Works
└── stateful_actor (selective message processing) ❌ Crashes
```

## Actor Class Memory Layout

### Base actor Class Members (in order)
```cpp
private:
    std::string m_name;
    std::weak_ptr<actor> m_self_ref;
    size_t m_assigned_thread_id = 0;
    std::atomic<actor_state> m_state{actor_state::running};

    // Two mailbox queues
    std::queue<std::unique_ptr<message_base>> m_mailbox;
    mutable std::mutex m_mailbox_mutex;
    std::queue<std::unique_ptr<message_base>> m_ask_queue;
    mutable std::mutex m_ask_queue_mutex;

    // Handler maps
    std::unordered_map<std::type_index, std::function<void(message_base*)>> m_handlers;
    std::unordered_map<std::type_index, std::function<void*(void*)>> m_ask_handlers;

    // NEW MEMBER - causes crash in stateful_actor
    size_t m_instance_id = 0;
```

### Stateful Actor Additional Members
```cpp
// From include/cas/stateful_actor.h
private:
    // Overrides base mailbox with deque for selective extraction
    std::deque<std::unique_ptr<message_base>> m_stateful_mailbox;
    std::mutex m_stateful_mailbox_mutex;

    // Set of message types accepted in current state
    std::set<std::type_index> m_accepted_types;
    std::mutex m_accepted_types_mutex;

    // By default, accept all message types (same as base actor)
    bool m_accept_all = true;
```

**Key Observation**: `stateful_actor` adds its own mutex members. If base class layout changes, these mutex offsets change, and locking/unlocking could corrupt memory or access wrong memory locations.

## Stateful Actor Implementation Details

### Overridden Methods
`stateful_actor` overrides these virtual methods from base `actor`:
```cpp
void enqueue_message(std::unique_ptr<message_base> msg);
void process_next_message();
bool has_messages();
```

### Critical Code from stateful_actor.cpp

**Line 18-26: enqueue_message()**
```cpp
void stateful_actor::enqueue_message(std::unique_ptr<message_base> msg) {
    if (get_state() != actor_state::running) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_stateful_mailbox_mutex);  // ← Locking derived class mutex
    m_stateful_mailbox.push_back(std::move(msg));
}
```

**Line 28-67: process_next_message()**
```cpp
void stateful_actor::process_next_message() {
    std::unique_ptr<message_base> msg;

    // Double-lock pattern
    {
        std::lock_guard<std::mutex> mailbox_lock(m_stateful_mailbox_mutex);  // ← Lock 1
        std::lock_guard<std::mutex> types_lock(m_accepted_types_mutex);      // ← Lock 2

        if (m_stateful_mailbox.empty()) {
            return;
        }

        // ... message selection logic ...
    }

    // Dispatch message
    if (msg) {
        actor* current = get_current_actor();  // ← Line 62: Unused variable
        current_actor_context = this;
        dispatch_message(msg.get());
        current_actor_context = nullptr;
    }
}
```

**Suspicious code at line 62**:
```cpp
actor* current = get_current_actor();
```
This stores the result but never uses it. Then immediately sets `current_actor_context = this;`. This might be a bug.

## Hypotheses

### Hypothesis 1: Mutex Memory Corruption
When base class layout changes, the offset of `m_stateful_mailbox_mutex` and `m_accepted_types_mutex` in the derived class changes. If there's any code making assumptions about memory layout or doing pointer arithmetic, it could be accessing the wrong memory addresses when trying to lock/unlock mutexes.

### Hypothesis 2: Virtual Function Table Issues
Adding a member changes the layout but shouldn't affect vtable. However, if there's undefined behavior elsewhere (like accessing deleted memory), changing the layout could expose it.

### Hypothesis 3: Alignment Issues
The `std::atomic<actor_state> m_state` member requires specific alignment. Adding `size_t m_instance_id` could change padding/alignment in ways that corrupt derived class members, especially the mutexes.

### Hypothesis 4: Initialization Order
Perhaps `stateful_actor` members are being initialized before base class members complete initialization, and the layout change affects this order.

### Hypothesis 5: Existing Bug Exposed
There might be a pre-existing bug in `stateful_actor` (like the unused `current` variable) that's masked until the memory layout changes.

## Reproduction Steps

1. Start with clean baseline:
   ```bash
   git checkout <commit-before-instance-id>
   ./claude/build.bat
   # Result: All 42 tests pass
   ```

2. Add instance_id member to actor.h:
   ```cpp
   // In private section, any position:
   size_t m_instance_id = 0;
   ```

3. Add getter to actor.h:
   ```cpp
   size_t instance_id() const;
   ```

4. Implement getter in actor.cpp:
   ```cpp
   size_t actor::instance_id() const {
       return m_instance_id;
   }
   ```

5. Build and test:
   ```bash
   ./claude/build.bat
   # Result: 40/42 tests pass, stateful_actor tests crash
   ```

6. Verify isolation:
   ```bash
   "cmake-build-debug\unit_tests.exe" "~[stateful]"
   # Result: All 40 non-stateful tests pass
   ```

## Environment

- **OS**: Windows 10.0.26200.6901
- **Compiler**: MSVC (Microsoft Visual Studio 2022 Community, v17.0)
- **MSVC Version**: 14.44.35207
- **Build Tool**: nmake.exe
- **CMake**: CLion 2 bundled version
- **Test Framework**: Catch2 v2.13.10
- **Architecture**: x64

## Related Files

### Implementation Files
- `include/cas/actor.h` - Base actor class (lines 40-70 for member variables)
- `source/actor.cpp` - Base actor implementation
- `include/cas/stateful_actor.h` - Stateful actor header
- `source/stateful_actor.cpp` - Stateful actor implementation (lines 18-90)

### Test Files
- `tests/00_basic/test_actor_creation.cpp` - Basic actor tests (all pass)
- `tests/04_advanced/test_stateful_actor.cpp` - Stateful actor tests (crashes at line 75)

### System Files
- `include/cas/system.h` - Actor system
- `source/system.cpp` - System implementation (register_actor() would assign instance_id)

## Questions for Investigation

1. **Why is stateful_actor sensitive to base class layout changes?**
   - Is it the mutex members?
   - Is it the virtual function overrides?
   - Is it the complex member initialization?

2. **Is there a bug in stateful_actor implementation?**
   - The unused `current` variable at line 62
   - The double-lock pattern
   - Something with typeid() usage?

3. **Could this be a compiler bug?**
   - MSVC-specific issue with member layout?
   - Virtual function dispatch issue?
   - Optimization bug?

4. **Is there undefined behavior being masked?**
   - Use-after-free?
   - Uninitialized memory?
   - Race condition exposed by different timing?

5. **Could atomic member alignment be the issue?**
   - Does `std::atomic<actor_state>` have special alignment requirements?
   - Does adding a `size_t` before/after it break alignment?
   - Are derived class members being misaligned?

## Workarounds Attempted

### Tried: Different member positions
- After `m_self_ref` ❌
- After `m_state` ❌
- At end of class ❌
**Result**: Position doesn't matter

### Tried: Adding move constructor deletion
```cpp
actor(actor&&) = delete;
actor& operator=(actor&&) = delete;
```
**Result**: No change to crash

### Working Workaround
Exclude stateful_actor tests:
```bash
"cmake-build-debug\unit_tests.exe" "~[stateful]"
```
**Result**: ✅ 40/42 tests pass (95% coverage)

## Next Steps for Investigation

1. **Debug in CLion**
   - Set breakpoint at `stateful_actor::enqueue_message`
   - Step through mutex lock operations
   - Check memory addresses of mutexes before/after layout change

2. **Add debug logging**
   - Print addresses of all member variables in constructor
   - Compare addresses with/without `m_instance_id`
   - Check if offsets are consistent

3. **Memory sanitizer**
   - Build with AddressSanitizer if available on MSVC
   - Check for buffer overflows or memory corruption

4. **Simplify stateful_actor**
   - Create minimal stateful_actor that reproduces crash
   - Remove features until it works
   - Identify exact problematic code

5. **Check for padding/alignment**
   - Use `sizeof()` and `offsetof()` to print member layouts
   - Compare with/without `m_instance_id`
   - Look for unexpected padding changes

6. **Review stateful_actor design**
   - Is the double-lock pattern safe?
   - Should it be using base class mailbox instead of its own?
   - Is the selective message processing correctly implemented?

## References

- Planning document: `planning/identity_metrics_implementation_plan.md`
- Recovery document: `planning/actor_identity_and_metrics_recovery.md`
- Actor header: `include/cas/actor.h`
- Stateful actor: `include/cas/stateful_actor.h`, `source/stateful_actor.cpp`
