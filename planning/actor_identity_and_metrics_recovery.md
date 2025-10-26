# Actor Identity and Metrics - Recovery Plan

## Context

We attempted to implement actor identity tracking and queue metrics based on `actor_naming_design.md`. The implementation compiled successfully and worked in standalone tests, but caused Catch2 unit tests to crash during static initialization. We reverted using CLion local history to restore a working state (all 42 tests passing).

## What Was Lost

### 1. Actor Identity Infrastructure

**Changes to `include/cas/actor.h`:**
- Added `size_t m_instance_id = 0;` member
- Added `std::type_index m_type_id{typeid(actor)};` member ← **PROBLEM: This caused crashes**
- Added public methods:
  - `size_t instance_id() const;`
  - `std::type_index type_id() const;`
  - `std::string type_name() const;`
  - `actor_identity identity() const;`

**Changes to `include/cas/system.h`:**
- Added `std::atomic<size_t> m_next_instance_id{1};` member
- Added `static size_t next_instance_id();` method
- Modified `create<>()` template to assign instance_id and type_id before registration

**Changes to `source/actor.cpp`:**
- Implemented all new getter methods
- Added platform-specific type name demangling (MSVC vs GCC/Clang)
- Identity struct construction logic

**Changes to `source/system.cpp`:**
- Implemented `next_instance_id()` using atomic increment

### 2. Actor Identity Struct

**Added to `include/cas/message_base.h`:**
```cpp
struct actor_identity {
    std::string name;
    std::type_index type_id{typeid(void)};  // Initially tried {typeid(void)}
    std::string type_name;
    size_t instance_id = 0;

    std::string to_string() const;  // Human-readable format
};
```

**Location rationale:** Placed in `message_base.h` to avoid circular dependencies between `actor.h` and message types.

### 3. Queue Metrics

**Added to `include/cas/actor.h`:**
- `size_t m_high_water_mark = 10000;` - configurable threshold
- `size_t m_peak_queue_depth = 0;` - maximum depth reached
- Public methods:
  - `size_t high_water_mark() const;`
  - `size_t peak_queue_depth() const;`
  - `void set_high_water_mark(size_t threshold);`

**Implementation plan (not completed):**
- Track queue depth in `enqueue_message()` and `enqueue_ask_message()`
- Update `m_peak_queue_depth` when depth increases
- Send `mailbox_high_water_mark` warning to system actor when threshold exceeded

### 4. System Message Infrastructure

**Added to `include/cas/message_base.h`:**
```cpp
struct system_message_base : message_base {
    actor_identity sender_id;  // Full identity for logging
    std::chrono::system_clock::time_point timestamp;
};

struct mailbox_high_water_mark : public system_message_base {
    size_t current_depth;
    size_t high_water_mark;
};
```

### 5. Enhanced Shutdown Reporting

**Planned (not implemented):**
- Collect statistics during shutdown (actor identity, queue depths, message counts)
- Enhanced shutdown log with actor details instead of just names
- Report which actors had undrained messages with full identity

### 6. Test Infrastructure

**Created but lost:**
- `tests/00_basic/test_actor_identity.cpp` - Basic tests for identity features
- `tests/simple_test.cpp` - Standalone test without Catch2 (proved features work)
- `build_simple.bat` - Build script for standalone test

## Root Cause Analysis

### The Crash

**Symptom:**
- Catch2 tests crashed at `TEST_CASE` macro line during static initialization
- Segmentation fault before any test code executed
- Only tests defining message structs (`struct X : public cas::message_base`) crashed
- Tests without custom messages worked fine

**Investigation findings:**
1. Simple standalone test (without Catch2) worked perfectly - all features functional
2. Some Catch2 tests passed (basic actor creation), others crashed
3. Crash pattern: Any test file with `struct message : cas::message_base` failed
4. Crash location: Always at TEST_CASE line (static registration phase)

**Suspected root causes:**

1. **`std::type_index m_type_id{typeid(actor)};` in actor.h header**
   - Default member initializer uses `typeid(actor)`
   - May cause issues when compiler processes derived classes during static init
   - RTTI evaluation order during static initialization is undefined

2. **Catch2 static test registration + RTTI interaction**
   - Catch2 registers tests statically using macros
   - May interact poorly with our RTTI-based initialization

3. **Possible ABI issues** (less likely since clean rebuild didn't help)

## Recovery Strategy - Step by Step

### Phase 1: Foundation (No RTTI)

**Goal:** Add basic identity WITHOUT std::type_index to avoid RTTI issues

**Step 1.1: Add instance_id only**
```cpp
// In actor.h private section
size_t m_instance_id = 0;

// In actor.h public section
size_t instance_id() const;
```

**Step 1.2: Test immediately**
- Build and run ALL tests
- If tests pass, commit: "Add actor instance_id tracking"
- If tests fail, investigate before proceeding

**Step 1.3: Add instance_id assignment in system::create**
```cpp
actor_ptr->m_instance_id = next_instance_id();
```

**Step 1.4: Test and commit**

### Phase 2: Type Information (Careful RTTI)

**Step 2.1: Add type_id WITHOUT default initializer**
```cpp
// In actor.h - NO default initializer!
std::type_index m_type_id;  // Initialize in constructor
```

**Problem:** This requires actor base class to have constructor, but it's currently default.

**Alternative approach:**
```cpp
// Use optional to defer initialization
std::optional<std::type_index> m_type_id;
```

**Step 2.2: Set type_id only in system::create**
```cpp
actor_ptr->m_type_id = typeid(ActorType);  // Or .emplace()
```

**Step 2.3: Test thoroughly**
- Run all tests multiple times
- If crashes occur, try different initialization strategies

### Phase 3: Type Name Demangling

**Step 3.1: Add type_name() method**
- Platform-specific demangling logic
- Test on both MSVC and GCC if possible

### Phase 4: Actor Identity Struct

**Step 4.1: Add minimal actor_identity to message_base.h**
```cpp
struct actor_identity {
    std::string name;
    std::string type_name;
    size_t instance_id = 0;
    // NO std::type_index yet
};
```

**Step 4.2: Add identity() method to actor**

**Step 4.3: Add type_id to actor_identity (if Phase 2 successful)**

### Phase 5: Queue Metrics

**Step 5.1: Add metric members**
- `m_high_water_mark`
- `m_peak_queue_depth`

**Step 5.2: Add getters/setters**

**Step 5.3: Implement tracking logic**
- Update in enqueue methods
- Compare current depth to peak, update if needed

### Phase 6: Warning Messages

**Step 6.1: Add system_message_base**

**Step 6.2: Add mailbox_high_water_mark message**

**Step 6.3: Send warnings when threshold exceeded**
- Requires system actor implementation (future work)

### Phase 7: Enhanced Shutdown Reporting

**Step 7.1: Collect actor statistics during shutdown**

**Step 7.2: Format enhanced shutdown log**

**Step 7.3: Include identity information in warnings**

## Testing Protocol

**After EACH step:**

1. Clean build: `rmdir /s /q cmake-build-debug`
2. Full rebuild: `./claude/build.bat`
3. Check all 42 tests pass
4. Run multiple times to ensure stability
5. If tests fail:
   - Note exactly which test fails
   - Note the error message
   - Revert the step
   - Try alternative approach
6. Only commit when tests pass

**Test categories to verify:**
- `[00_basic]` - Basic actor creation (9 tests)
- `[01_simple]` - Message structs and handlers (9 tests)
- `[02_interactions]` - Actor communication (3 tests)
- `[03_lifecycle]` - Lifecycle hooks (5 tests)
- `[04_advanced]` - Fast/inline/stateful actors (12 tests)

## Lessons Learned

1. **Always commit before major changes** - Local history saved us but we lost work
2. **Default member initializers with RTTI are dangerous** - Avoid `{typeid(T)}` in headers
3. **Test after each small change** - Don't batch multiple features
4. **Static initialization order is fragile** - Be careful with Catch2 and RTTI
5. **Standalone tests are valuable** - Helped prove code works, isolated Catch2 issue
6. **std::type_index requires initialization** - Can't default construct, need explicit init

## Alternative Approaches to Consider

### Option A: Avoid std::type_index entirely
- Store type name string only (from `typeid().name()`)
- Less type-safe but simpler
- No RTTI initialization issues

### Option B: Lazy initialization
- Don't initialize type_id in constructor
- Initialize on first access via getter
- More runtime overhead but safer

### Option C: Use virtual method for type info
- Add `virtual std::type_index get_type_id() const = 0;`
- Each derived class implements
- No static initialization issues
- Requires more boilerplate in derived classes

### Option D: Macro-based registration
- `REGISTER_ACTOR_TYPE(MyActor)` macro
- Macro creates static initializer that's safe
- Similar to how Catch2 registers tests

## Next Steps

1. Review this document with user
2. Confirm recovery strategy
3. Start with Phase 1, Step 1.1
4. Test rigorously after each step
5. Commit frequently with descriptive messages
6. Document any new issues encountered

## References

- Original design: `planning/actor_naming_design.md`
- Simple test that worked: `tests/simple_test.cpp` (if we can recover from history)
- Build process: `claude/build.bat`
