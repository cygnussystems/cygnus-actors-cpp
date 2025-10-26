# Actor Identity & Metrics Implementation Plan

## Overview

This document outlines the step-by-step implementation of actor identity tracking and queue metrics for the Cygnus Actor Framework. The plan emphasizes defensive development practices to avoid the static initialization crashes we encountered previously.

## Key Decisions

1. **System Actor:** The existing `system` class will handle metric messages directly (no separate actor needed)
2. **Type Storage:** Store type names as strings only - avoid `std::type_index` member variables to prevent static initialization issues
3. **Name Uniqueness:** Allow duplicate names by default, with optional enforcement via config flag
4. **Default Naming:** Actors can auto-name themselves using pattern: `TypeName#instance_id`

## Defensive Development Principles

### Testing Protocol (After EVERY Step)
1. Clean build: `cmd /c "if exist cmake-build-debug rmdir /s /q cmake-build-debug"`
2. Full rebuild: `./claude/build.bat`
3. Verify: All 42 tests must pass
4. Run tests 2-3 times to ensure stability
5. **If ANY test fails:** Revert immediately, analyze, try different approach
6. **Only commit when tests are green**

### Safety Rules
- ❌ **Never use default initializers with typeid()** - Only assign in `system::create()`
- ✅ **String storage for type names** - Avoid `std::type_index` member variables
- ✅ **Small incremental changes** - One feature at a time
- ✅ **Test every change** - Never batch changes before testing
- ✅ **Commit frequently** - Each passing phase gets a commit

## Implementation Phases

### Phase 1: Instance ID (Simple, Safe)

**What:** Add unique sequential ID to each actor

**Files Modified:**
- `include/cas/actor.h`
- `source/actor.cpp`
- `include/cas/system.h`
- `source/system.cpp`

**Changes:**

**Step 1.1:** Add to `actor.h` private section (after `m_self_ref`):
```cpp
size_t m_instance_id = 0;  // Unique sequential ID assigned by system
```

**Step 1.2:** Add to `actor.h` public section:
```cpp
// Get unique instance ID
size_t instance_id() const;
```

**Step 1.3:** Implement in `actor.cpp`:
```cpp
size_t actor::instance_id() const {
    return m_instance_id;
}
```

**Step 1.4:** Add to `system.h` private section (after `m_next_message_id`):
```cpp
// Instance ID counter (globally unique across all actors)
std::atomic<size_t> m_next_instance_id{1};  // Start at 1, 0 means "unassigned"
```

**Step 1.5:** Add to `system.h` public section:
```cpp
// Internal: get next unique instance ID
static size_t next_instance_id();
```

**Step 1.6:** Implement in `system.cpp`:
```cpp
size_t system::next_instance_id() {
    return instance().m_next_instance_id.fetch_add(1);
}
```

**Step 1.7:** Modify `system::create()` template in `system.h` (before `register_actor()`):
```cpp
// Assign unique instance ID
actor_ptr->m_instance_id = next_instance_id();
```

**Test Criteria:**
- All 42 tests pass
- `instance_id()` returns unique values for different actors
- IDs are sequential starting from 1

**Commit Message:** `"Add actor instance_id tracking"`

---

### Phase 2: Type Name (String Only - Simple)

**What:** Store the class name of each actor as a string

**Files Modified:**
- `include/cas/actor.h`
- `source/actor.cpp`
- `include/cas/system.h`

**Changes:**

**Step 2.1:** Add to `actor.h` private section (after `m_instance_id`):
```cpp
std::string m_type_name;  // Demangled class name
```

**Step 2.2:** Add to `actor.h` public section:
```cpp
// Get demangled type name
const std::string& type_name() const;
```

**Step 2.3:** Implement in `actor.cpp`:
```cpp
const std::string& actor::type_name() const {
    return m_type_name;
}
```

**Step 2.4:** Add helper function to `actor.cpp` (static or in anonymous namespace):
```cpp
#ifndef _MSC_VER
#include <cxxabi.h>
#endif

namespace {
    std::string demangle_type_name(const char* mangled_name) {
#ifdef _MSC_VER
        // MSVC already provides demangled names
        return mangled_name;
#else
        // GCC/Clang need demangling
        int status;
        char* demangled = abi::__cxa_demangle(mangled_name, nullptr, nullptr, &status);
        std::string result = (status == 0) ? demangled : mangled_name;
        free(demangled);
        return result;
#endif
    }
}
```

**Step 2.5:** Modify `system::create()` template in `system.h` (after instance_id assignment):
```cpp
// Assign type name (demangled)
actor_ptr->m_type_name = demangle_type_name(typeid(ActorType).name());
```

**Note:** Need to include `<typeinfo>` in system.h and potentially move demangling to system.cpp

**Test Criteria:**
- All 42 tests pass
- `type_name()` returns readable class names (not mangled on GCC/Clang)
- Type name matches the actual class

**Commit Message:** `"Add actor type_name tracking"`

---

### Phase 3: Actor Identity Struct

**What:** Unified struct containing all actor identification info

**Files Modified:**
- `include/cas/message_base.h`
- `include/cas/actor.h`
- `source/actor.cpp`

**Changes:**

**Step 3.1:** Add to `message_base.h` (after `message_base` struct, before `system_shutdown`):
```cpp
// Actor identity for logging and system messages
// Contains all actor identification information in one place
struct actor_identity {
    std::string name;        // User-assigned name (empty if unnamed)
    std::string type_name;   // Demangled class name
    size_t instance_id = 0;  // System-assigned unique ID

    // Human-readable representation
    // Format: "name (TypeName#id)" or "TypeName#id" if unnamed
    std::string to_string() const {
        if (!name.empty()) {
            return name + " (" + type_name + "#" + std::to_string(instance_id) + ")";
        } else {
            return type_name + "#" + std::to_string(instance_id);
        }
    }
};
```

**Step 3.2:** Add to `actor.h` public section:
```cpp
// Get full actor identity (for logging and system messages)
actor_identity identity() const;
```

**Step 3.3:** Implement in `actor.cpp`:
```cpp
actor_identity actor::identity() const {
    return actor_identity{
        m_name,
        m_type_name,
        m_instance_id
    };
}
```

**Test Criteria:**
- All 42 tests pass
- `identity().to_string()` produces readable format
- Named actors show name, unnamed show just type#id

**Commit Message:** `"Add actor_identity struct"`

---

### Phase 4: Queue Metrics Infrastructure

**What:** Add metrics tracking for mailbox queue depth

**Files Modified:**
- `include/cas/actor.h`
- `source/actor.cpp`

**Changes:**

**Step 4.1:** Add to `actor.h` private section (after `m_ask_handlers`):
```cpp
// Queue metrics for monitoring
size_t m_high_water_mark = 10000;  // Configurable threshold for queue warnings
size_t m_peak_queue_depth = 0;     // Maximum queue depth reached during lifetime
```

**Step 4.2:** Add to `actor.h` public section:
```cpp
// Get peak queue depth reached
size_t peak_queue_depth() const;

// Get high water mark threshold
size_t high_water_mark() const;

// Set high water mark threshold (can be called in on_start)
void set_high_water_mark(size_t threshold);
```

**Step 4.3:** Implement in `actor.cpp`:
```cpp
size_t actor::peak_queue_depth() const {
    return m_peak_queue_depth;
}

size_t actor::high_water_mark() const {
    return m_high_water_mark;
}

void actor::set_high_water_mark(size_t threshold) {
    m_high_water_mark = threshold;
}
```

**Test Criteria:**
- All 42 tests pass
- Default high_water_mark is 10000
- peak_queue_depth starts at 0
- Can set custom high_water_mark

**Commit Message:** `"Add queue metrics infrastructure"`

---

### Phase 5: Track Peak Queue Depth

**What:** Update peak depth tracking in message enqueue methods

**Files Modified:**
- `source/actor.cpp`

**Changes:**

**Step 5.1:** Modify `actor::enqueue_message()` in `actor.cpp` (after acquiring lock):
```cpp
void actor::enqueue_message(std::unique_ptr<message_base> msg) {
    // Don't accept new messages if actor is stopping or stopped
    if (m_state.load() != actor_state::running) {
        return;  // Silently drop message
    }

    std::lock_guard<std::mutex> lock(m_mailbox_mutex);
    m_mailbox.push(std::move(msg));

    // Track peak queue depth
    size_t current_depth = m_mailbox.size();
    if (current_depth > m_peak_queue_depth) {
        m_peak_queue_depth = current_depth;
    }
}
```

**Step 5.2:** Modify `actor::enqueue_ask_message()` similarly:
```cpp
void actor::enqueue_ask_message(std::unique_ptr<message_base> msg) {
    // Don't accept new messages if actor is stopping or stopped
    if (m_state.load() != actor_state::running) {
        return;  // Silently drop message
    }

    std::lock_guard<std::mutex> lock(m_ask_queue_mutex);
    m_ask_queue.push(std::move(msg));

    // Track peak queue depth
    size_t current_depth = m_ask_queue.size();
    if (current_depth > m_peak_queue_depth) {
        m_peak_queue_depth = current_depth;
    }
}
```

**Note:** We're tracking the combined peak across both queues in the same `m_peak_queue_depth` counter.

**Test Criteria:**
- All 42 tests pass
- Peak depth increases when messages are enqueued
- Peak depth doesn't decrease when messages are processed

**Commit Message:** `"Track peak queue depth in enqueue methods"`

---

### Phase 6: System Message Types

**What:** Define message types for system notifications

**Files Modified:**
- `include/cas/message_base.h`

**Changes:**

**Step 6.1:** Add to `message_base.h` (after `actor_identity` struct):
```cpp
// Base struct for system messages that include full actor identity
// Used for internal system notifications that need detailed sender information
struct system_message_base : message_base {
    actor_identity sender_id;  // Full identity of the actor sending this system message
    std::chrono::system_clock::time_point timestamp;
};

// System message sent when an actor's mailbox exceeds high water mark
// Sent to system for logging and monitoring
struct mailbox_high_water_mark : public system_message_base {
    size_t current_depth;      // Current queue depth when warning triggered
    size_t high_water_mark;    // Configured threshold
};
```

**Test Criteria:**
- All 42 tests pass (just adding struct definitions)
- No runtime changes, just type definitions

**Commit Message:** `"Add system message types for metrics"`

---

### Phase 7: Send High Water Mark Warnings

**What:** Detect when mailbox exceeds threshold and send warnings

**Files Modified:**
- `source/actor.cpp`
- `include/cas/system.h` (optional - for system to handle messages)

**Changes:**

**Step 7.1:** Modify enqueue methods to detect threshold crossing:

In `actor::enqueue_message()` (after tracking peak):
```cpp
// Check if we just crossed the high water mark threshold
if (current_depth > m_high_water_mark &&
    (current_depth - 1) <= m_high_water_mark) {
    // Just crossed threshold - send warning
    // TODO: Send mailbox_high_water_mark message to system
    // For now, just log to stderr
    std::cerr << "WARNING: Actor " << identity().to_string()
              << " mailbox exceeded threshold: " << current_depth
              << " > " << m_high_water_mark << "\n";
}
```

**Step 7.2:** (Optional) Add system handler for warnings
- This can be deferred to later when we implement proper system actor
- For now, the stderr warning is sufficient

**Test Criteria:**
- All 42 tests pass
- Warning messages appear when threshold exceeded (if we create a test for it)
- Warning only sent once when crossing threshold, not every message after

**Commit Message:** `"Warn when mailbox exceeds high water mark"`

---

### Phase 8: Optional Name Uniqueness Enforcement

**What:** Add optional enforcement of unique actor names

**Files Modified:**
- `include/cas/system.h`
- `source/actor.cpp`

**Changes:**

**Step 8.1:** Add config option to `system_config` in `system.h`:
```cpp
struct system_config {
    size_t thread_pool_size = 0;  // 0 = auto-detect
    bool enforce_unique_names = false;  // If true, throw on duplicate names
};
```

**Step 8.2:** Modify `actor::set_name()` in `actor.cpp`:
```cpp
void actor::set_name(const std::string& name) {
    // Check for uniqueness if enforcement enabled
    if (get_system()->config().enforce_unique_names) {
        auto existing = actor_registry::get(name);
        if (existing.is_valid()) {
            throw std::runtime_error("Actor name '" + name + "' already exists");
        }
    }

    m_name = name;

    // Register with actor registry for name-based lookup
    if (auto shared = m_self_ref.lock()) {
        actor_registry::register_actor(name, shared);
    }
}
```

**Step 8.3:** Add public config accessor to system class:
```cpp
const system_config& config() const { return m_config; }
```

**Test Criteria:**
- All 42 tests pass (with default config allowing duplicates)
- Add test for uniqueness enforcement (enabled via config)
- Exception thrown when duplicate name set with enforcement on

**Commit Message:** `"Add optional actor name uniqueness enforcement"`

---

### Phase 9: Enhanced Shutdown Reporting

**What:** Include full actor identity in shutdown warnings

**Files Modified:**
- `source/system.cpp`

**Changes:**

**Step 9.1:** Modify shutdown log generation in `system::shutdown()`:

Replace existing warning generation (around line 165-170) with:
```cpp
size_t remaining = actor_ptr->queue_size();
if (remaining > 0) {
    std::string warning = "Actor " + actor_ptr->identity().to_string() +
                         " has " + std::to_string(remaining) +
                         " unprocessed messages at shutdown";
    std::lock_guard<std::mutex> log_lock(inst.m_shutdown_log_mutex);
    inst.m_shutdown_log.push_back(warning);
}
```

**Step 9.2:** Similar for fast actors (around line 180):
```cpp
size_t remaining = actor_ptr->queue_size();
if (remaining > 0) {
    std::string warning = "Actor " + actor_ptr->identity().to_string() +
                         " has " + std::to_string(remaining) +
                         " unprocessed messages at shutdown";
    std::lock_guard<std::mutex> log_lock(inst.m_shutdown_log_mutex);
    inst.m_shutdown_log.push_back(warning);
}
```

**Test Criteria:**
- All 42 tests pass
- Shutdown warnings now include type name and instance ID
- More informative error messages

**Commit Message:** `"Enhanced shutdown reporting with full actor identity"`

---

## Testing Strategy Summary

### After Each Phase:

1. **Clean build:**
   ```bash
   cmd /c "if exist cmake-build-debug rmdir /s /q cmake-build-debug"
   ```

2. **Full rebuild:**
   ```bash
   ./claude/build.bat
   ```

3. **Expected output:**
   ```
   All tests passed (107 assertions in 42 test cases)
   ```

4. **If tests fail:**
   - Note which test failed
   - Note the exact error
   - Revert changes immediately
   - Analyze root cause
   - Try alternative approach

5. **When tests pass:**
   - Run tests 2-3 more times to verify stability
   - Create git commit with descriptive message
   - Move to next phase

### Test Categories to Monitor:
- `[00_basic]` - Basic actor creation (9 tests)
- `[01_simple]` - Message structs and handlers (9 tests)
- `[02_interactions]` - Actor communication (3 tests)
- `[03_lifecycle]` - Lifecycle hooks (5 tests)
- `[04_advanced]` - Fast/inline/stateful actors (12 tests)

## Lessons from Previous Failure

### What Went Wrong:
1. Used `std::type_index m_type_id{typeid(actor)};` as default member initializer
2. This caused static initialization crashes in Catch2 tests
3. Tests defining message structs crashed at TEST_CASE line
4. Lost work because we hadn't committed

### How We're Avoiding It:
1. ✅ String storage for type info instead of std::type_index members
2. ✅ Only assign type info in system::create(), not in headers
3. ✅ Test after each small change
4. ✅ Commit frequently when tests pass
5. ✅ Use .gitignore to exclude build artifacts

## Success Criteria

All phases complete when:
- ✅ All 42 tests pass consistently
- ✅ Each actor has unique instance_id
- ✅ Type names are readable (demangled)
- ✅ actor_identity struct works correctly
- ✅ Peak queue depth is tracked
- ✅ High water mark warnings are sent
- ✅ Name uniqueness can be optionally enforced
- ✅ Shutdown reports include full identity
- ✅ All changes committed to git

## References

- Recovery plan: `planning/actor_identity_and_metrics_recovery.md`
- Original design: `planning/actor_naming_design.md`
- Build process: `claude/build.bat`
