# C++ Development Workflow and Coding Standards

## Overview
This document describes the complete development workflow, build system setup, coding standards, and testing practices used in this project. Use this as a template for other C++ projects.

---

## Build System Setup

### Compiler and Toolchain
- **Compiler**: MSVC (Microsoft Visual C++)
- **Version**: 14.44.35207 (Visual Studio 2022 Community)
- **Platform**: x64 (64-bit)
- **Build Tool**: nmake.exe (Visual Studio's make)
- **CMake**: CLion bundled version (`C:\Users\ritte\AppData\Local\Programs\CLion 2\bin\cmake\win\x64\bin\cmake.exe`)

### Visual Studio Paths
```
Base: C:\Program Files\Microsoft Visual Studio\2022\Community
MSVC: C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207
vcvarsall.bat: C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat
```

### Environment Setup
Building from command line requires these environment variables (set by `vcvarsall.bat x64`):

1. **PATH** - Must include:
   - MSVC compiler tools (cl.exe, nmake.exe, link.exe)
   - Windows SDK tools
   - Visual Studio build tools

2. **INCLUDE** - C++ standard library and Windows SDK headers:
   ```
   C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207\include
   C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\ucrt
   C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\shared
   C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\um
   ```

3. **LIB** - Standard library and Windows SDK libraries:
   ```
   C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207\lib\x64
   C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0\ucrt\x64
   C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0\um\x64
   ```

### Build Script Template
Create `claude/build.bat` in your project root:

```batch
@echo off
setlocal

echo Setting up Visual Studio environment...
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
if %ERRORLEVEL% neq 0 (
    echo ERROR: Failed to set up Visual Studio environment
    exit /b 1
)

echo.
echo Configuring CMake...
"C:\Users\ritte\AppData\Local\Programs\CLion 2\bin\cmake\win\x64\bin\cmake.exe" -S . -B cmake-build-debug -G "NMake Makefiles"
if %ERRORLEVEL% neq 0 (
    echo ERROR: CMake configuration failed
    exit /b 1
)

echo.
echo Building project...
"C:\Users\ritte\AppData\Local\Programs\CLion 2\bin\cmake\win\x64\bin\cmake.exe" --build cmake-build-debug --target unit_tests
if %ERRORLEVEL% neq 0 (
    echo ERROR: Build failed
    exit /b 1
)

echo.
echo Build successful!
echo.
echo Running unit tests...
cmake-build-debug\unit_tests.exe
if %ERRORLEVEL% neq 0 (
    echo.
    echo ERROR: Tests failed
    exit /b 1
)

echo.
echo All tests passed!
endlocal
```

### CMakeLists.txt Template
```cmake
cmake_minimum_required(VERSION 3.20)
project(YourProjectName)

# C++17 standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# MSVC specific flags
if(MSVC)
    # /EHsc - Enable C++ exception handling
    # /W4 - Warning level 4
    # /permissive- - Standards conformance mode
    add_compile_options(/EHsc /W4 /permissive-)
endif()

# Header-only external libraries go in include/external/
include_directories(include)
include_directories(include/external)

# Source files
set(SOURCES
    source/file1.cpp
    source/file2.cpp
    # Add all .cpp files
)

# Create library
add_library(YourProjectName STATIC ${SOURCES})

# Test executable
file(GLOB_RECURSE TEST_SOURCES "tests/*.cpp")
add_executable(unit_tests ${TEST_SOURCES})
target_link_libraries(unit_tests YourProjectName)
```

### Building the Project

#### Option 1: Build Script (Recommended for Command Line)
```bash
./claude/build.bat
```

This automatically:
1. Sets up Visual Studio environment
2. Configures CMake
3. Builds the project
4. Runs tests

#### Option 2: CLion IDE
- Open project in CLion
- CLion automatically detects CMake configuration
- Use `Build > Build 'unit_tests'` or `Ctrl+F9`
- CLion handles environment setup automatically

#### Option 3: Developer Command Prompt
```cmd
# Open "Developer Command Prompt for VS 2022" from Start Menu
cd C:\path\to\your\project
cmake --build cmake-build-debug --target unit_tests
```

---

## Project Structure

```
YourProject/
├── claude/                     # Claude Code working directory
│   ├── build.bat              # Build script (sets up VS environment)
│   └── planning/              # Design docs and planning
│       ├── feature_name.md    # Feature design documents
│       ├── bug_name.md        # Bug investigation notes
│       └── research_topic.md  # Research and analysis docs
├── include/                   # Public headers
│   ├── yournamespace/         # Your library headers
│   │   ├── class1.h
│   │   ├── class2.h
│   │   └── yournamespace.h    # Main include (includes all)
│   └── external/              # Header-only third-party libraries
│       ├── catch.hpp          # Testing framework
│       └── concurrentqueue.h  # Lock-free queue, etc.
├── source/                    # Implementation files
│   ├── class1.cpp
│   ├── class2.cpp
│   └── ...
├── tests/                     # Test files organized by category
│   ├── test_common.h          # Shared test utilities
│   ├── 00_basic/
│   │   ├── test_feature1.cpp
│   │   └── test_feature2.cpp
│   ├── 01_advanced/
│   │   └── test_feature3.cpp
│   └── ...
├── planning/                  # User planning directory
│   └── ...                    # User's own notes and docs
├── cmake-build-debug/         # Build output (gitignored)
│   ├── YourProjectName.lib
│   └── unit_tests.exe
├── CMakeLists.txt             # CMake configuration
├── CMakePresets.json          # CMake presets (optional)
├── .gitignore                 # Git ignore file
└── CLAUDE.md                  # Claude Code instructions
```

---

## Testing Framework

### Catch2 (Header-Only)
We use **Catch2 v2.x** (single-header version) for testing.

#### Download and Setup
```bash
# Download catch.hpp (v2.13.10)
curl -L https://github.com/catchorg/Catch2/releases/download/v2.13.10/catch.hpp -o include/external/catch.hpp
```

#### Test File Template
```cpp
// tests/test_common.h
#ifndef TEST_COMMON_H
#define TEST_COMMON_H

// Include Catch2 ONCE with main
#define CATCH_CONFIG_MAIN
#include "../include/external/catch.hpp"

// Common test utilities
#include <thread>
#include <chrono>

inline void wait_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// Cleanup macro for tests that use your system
#define TEST_CLEANUP() \
    do { \
        your_namespace::system::shutdown(); \
        your_namespace::system::wait_for_shutdown(); \
        your_namespace::system::reset(); \
    } while(0)

#endif // TEST_COMMON_H
```

```cpp
// tests/00_basic/test_feature.cpp
#include "../test_common.h"
#include "yournamespace/yournamespace.h"

TEST_CASE("Feature description", "[category][subcategory]") {
    // Arrange
    auto obj = create_test_object();

    // Act
    auto result = obj.do_something();

    // Assert
    REQUIRE(result == expected_value);

    // Cleanup (if needed)
    TEST_CLEANUP();
}

TEST_CASE("Another test", "[category]") {
    // Test sections for related tests
    SECTION("First case") {
        REQUIRE(1 + 1 == 2);
    }

    SECTION("Second case") {
        REQUIRE(2 * 2 == 4);
    }
}
```

#### Running Tests
```bash
# Run all tests
./cmake-build-debug/unit_tests.exe

# Run tests by tag
./cmake-build-debug/unit_tests.exe "[category]"

# Run with compact reporter
./cmake-build-debug/unit_tests.exe --reporter compact

# List all tests
./cmake-build-debug/unit_tests.exe --list-tests
```

#### Test Organization
- Use hierarchical tags: `[00_basic][creation]`, `[01_advanced][threading]`
- Number prefixes control execution order
- Each test file focuses on one feature/component
- Use descriptive test names: "Feature does X when Y happens"

---

## Coding Standards

### Naming Conventions

#### Rule: Use `snake_case` for ALL identifiers
```cpp
// Classes and structs
class actor_system {};
struct message_base {};

// Functions and methods
void process_message();
int calculate_result();

// Variables
int thread_count = 4;
std::string actor_name;

// Constants
const int max_threads = 16;
constexpr size_t buffer_size = 1024;

// Namespaces
namespace cas {}
namespace your_project {}
```

#### Member Variables: Use `m_` prefix
```cpp
class actor {
private:
    std::string m_name;              // ✅ Good
    size_t m_thread_id;              // ✅ Good
    std::atomic<bool> m_running;     // ✅ Good

    // NOT: name_, m_name_, thread_id_
};
```

#### Struct Public Members: NO prefix
```cpp
struct message_base {
    uint64_t id;           // ✅ Good - plain name
    actor_ref sender;      // ✅ Good - plain name

    // NOT: m_id, m_sender (structs are data holders)
};
```

#### Accessor Methods: Property-style (NO `get_`/`set_`)
```cpp
class actor {
public:
    // ✅ Good - property-style
    const std::string& name() const { return m_name; }
    void name(const std::string& n) { m_name = n; }

    size_t thread_id() const { return m_thread_id; }

    // ❌ Bad - Java-style
    // const std::string& get_name() const;
    // void set_name(const std::string& n);
};
```

#### Exception: Use `get` for smart pointer-like operations
```cpp
// ✅ Good - matches std::unique_ptr<T>::get()
template<typename T>
T* actor_ref::get() {
    return dynamic_cast<T*>(m_actor.get());
}

// ✅ Good - checked version
template<typename T>
T* actor_ref::get_checked() {
    T* ptr = get<T>();
    if (!ptr) throw std::runtime_error("Type mismatch");
    return ptr;
}
```

### Code Organization

#### Implement functions in .cpp files (NOT headers)
```cpp
// actor.h
class actor {
public:
    const std::string& name() const;  // Declaration only
    void set_state(actor_state state);
};

// actor.cpp
const std::string& actor::name() const {
    if (m_name.empty()) {
        const_cast<actor*>(this)->m_name = generate_default_name();
    }
    return m_name;
}

void actor::set_state(actor_state state) {
    m_state.store(state);
}
```

**Exception**: Template functions must stay in headers:
```cpp
// system.h
template<typename ActorType, typename... Args>
actor_ref system::create(Args&&... args) {
    auto actor_ptr = std::make_shared<ActorType>(std::forward<Args>(args)...);
    register_actor(actor_ptr);
    return actor_ref(actor_ptr);
}
```

#### Documentation: Headers only (Doxygen style)
```cpp
// actor.h
/// Base class for all actors.
/// Users inherit from this and override lifecycle hooks.
class actor {
public:
    /// Get the actor's name (user-set or auto-generated).
    /// @return Reference to actor name
    const std::string& name() const;

    /// Set the actor's name and register with actor_registry.
    /// @param name The name to assign
    void set_name(const std::string& name);
};

// actor.cpp - NO documentation needed
const std::string& actor::name() const {
    // Implementation without comments
}
```

### Struct vs Class

#### Use `struct` for data holders
```cpp
// ✅ Good - simple data
struct message_base {
    uint64_t id;
    actor_ref sender;
    virtual ~message_base() = default;
};

struct ping_msg : public message_base {
    int sequence_number;
};
```

#### Use `class` for types with behavior and invariants
```cpp
// ✅ Good - has behavior and encapsulation
class actor {
private:
    std::string m_name;
    std::atomic<actor_state> m_state;

protected:
    virtual void on_start() = 0;

public:
    const std::string& name() const;
    actor_state state() const;
};
```

### API Design Philosophy

#### Match standard library conventions
```cpp
// ✅ Good - matches std::thread, std::mutex, etc.
class thread_pool {
public:
    void start();          // Like std::thread
    void join();           // Like std::thread
    size_t size() const;   // Like std::vector
};

// ✅ Good - matches std::unique_ptr, std::shared_ptr
class actor_ref {
public:
    template<typename T>
    T* get() const;  // Like smart pointers

    explicit operator bool() const;  // Like smart pointers
};
```

#### Avoid exposing raw pointers in public APIs
```cpp
// ✅ Good - use references or smart pointers
class system {
public:
    static actor_ref create_actor();  // Returns handle, not raw pointer
};

// ❌ Bad - raw pointer in public API
class system {
public:
    static actor* create_actor();  // Unclear ownership
};
```

#### RAII and Resource Management
```cpp
// ✅ Good - RAII for lifecycle management
class system {
public:
    static void start();              // Acquire resources
    static void shutdown();           // Initiate cleanup
    static void wait_for_shutdown();  // Wait for completion
    ~system() {                       // Destructor ensures cleanup
        if (m_running) {
            shutdown();
            wait_for_shutdown();
        }
    }
};
```

### Style Notes

#### Braces and Formatting
```cpp
// ✅ Good - consistent brace style
void function() {
    if (condition) {
        do_something();
    } else {
        do_other_thing();
    }
}

class my_class {
public:
    void method() {
        // Implementation
    }
};
```

#### Auto and Type Deduction
```cpp
// ✅ Good - use auto for complex types
auto it = map.find(key);
auto actor_ptr = std::make_shared<my_actor>();

// ✅ Good - explicit for simple types
int count = 0;
bool running = true;
```

#### Const Correctness
```cpp
// ✅ Good - const for read-only operations
class actor {
public:
    const std::string& name() const;  // Const method
    size_t thread_id() const;         // Returns by value, still const

private:
    std::string m_name;
    const size_t m_thread_id;         // Const member (immutable after construction)
};
```

---

## Third-Party Libraries

### Header-Only Libraries (Recommended)
Place in `include/external/`:
- **Catch2 v2.13.10** - Testing framework
- **moodycamel::ConcurrentQueue** - Lock-free MPMC queue
- Single-file libraries preferred for simplicity

### How to Integrate
```bash
# 1. Download header
curl -L <url> -o include/external/library_name.h

# 2. Include in your code
#include "external/library_name.h"

# 3. No linking needed (header-only)
```

### Lock-Free Queue Example (moodycamel)
```bash
# Download
curl -L https://raw.githubusercontent.com/cameron314/concurrentqueue/master/concurrentqueue.h -o include/external/concurrentqueue.h

# Use
#include "external/concurrentqueue.h"

moodycamel::ConcurrentQueue<std::unique_ptr<message>> queue;
queue.enqueue(std::move(msg));

std::unique_ptr<message> msg;
if (queue.try_dequeue(msg)) {
    // Process message
}
```

---

## CLion Integration

### CMake Configuration
CLion automatically:
1. Detects `CMakeLists.txt`
2. Sets up MSVC toolchain
3. Configures environment variables
4. Provides IntelliSense/code completion

### Build Configurations
- **Debug** (default): Full symbols, no optimization
- **Release**: Optimizations enabled, minimal symbols

### Keyboard Shortcuts
- `Ctrl+F9` - Build project
- `Shift+F10` - Run tests
- `Ctrl+Shift+F10` - Run current test file
- `Alt+Shift+F10` - Select run configuration

### Running Tests in CLion
1. Right-click test file → Run
2. Use Run gutter icons next to TEST_CASE
3. View test results in integrated test runner

---

## Git Configuration

### .gitignore Template
```gitignore
# Build output
cmake-build-*/
build/
*.exe
*.lib
*.obj
*.pdb
*.ilk

# IDE files
.idea/
.vs/
*.user
*.suo

# Temporary files
*.tmp
*.log

# OS files
.DS_Store
Thumbs.db
```

### Recommended Git Workflow
```bash
# 1. Feature branch
git checkout -b feature/new-feature

# 2. Make changes and commit
git add .
git commit -m "Add new feature: description

- Detailed change 1
- Detailed change 2

🤖 Generated with [Claude Code](https://claude.com/claude-code)

Co-Authored-By: Claude <noreply@anthropic.com>"

# 3. Merge to main
git checkout main
git merge feature/new-feature
```

---

## Claude Code Instructions (CLAUDE.md)

Create `CLAUDE.md` in project root:

```markdown
# Claude Code Notes for YOUR_PROJECT_NAME

## Quick Reference

### Building the Project
```bash
./claude/build.bat
```

### Running Tests
```bash
./cmake-build-debug/unit_tests.exe

# Run specific category
./cmake-build-debug/unit_tests.exe "[category]"
```

## Project Structure
[Describe your project structure]

## Coding Standards
See planning/development_workflow_and_standards.md for complete standards.

### Quick Rules
- **ALL identifiers**: `snake_case`
- **Member variables**: `m_` prefix (e.g., `m_name`)
- **Struct members**: NO prefix (e.g., `sender`)
- **Accessor methods**: Property-style (e.g., `name()`, NOT `get_name()`)
- **Implementation**: In `.cpp` files, NOT headers (except templates)
- **Documentation**: In headers only (Doxygen style)

## Known Issues
[Document any known bugs or limitations]
```

---

## Build Troubleshooting

### Common Issues

#### 1. "cl.exe not found" or similar compiler errors
**Cause**: Environment not set up
**Solution**: Use `claude/build.bat` which calls `vcvarsall.bat`

#### 2. "INCLUDE environment variable not set"
**Cause**: Building outside Developer Command Prompt without environment setup
**Solution**: Use `claude/build.bat` or open Developer Command Prompt

#### 3. CMake can't find compiler
**Cause**: CMake not finding MSVC
**Solution**:
```bash
# Specify generator explicitly
cmake -G "NMake Makefiles" -S . -B cmake-build-debug
```

#### 4. Tests hang when run from command line
**Cause**: Debug breakpoints in code
**Solution**: Remove all breakpoints or use Release build

### Verification Script
```batch
@echo off
echo Checking build environment...

where cl.exe >nul 2>&1
if %ERRORLEVEL% equ 0 (
    echo ✓ cl.exe found
) else (
    echo ✗ cl.exe not found - run vcvarsall.bat
)

where nmake.exe >nul 2>&1
if %ERRORLEVEL% equ 0 (
    echo ✓ nmake.exe found
) else (
    echo ✗ nmake.exe not found - run vcvarsall.bat
)

if defined INCLUDE (
    echo ✓ INCLUDE set
) else (
    echo ✗ INCLUDE not set - run vcvarsall.bat
)

if defined LIB (
    echo ✓ LIB set
) else (
    echo ✗ LIB not set - run vcvarsall.bat
)
```

---

## Performance Considerations

### Lock-Free Data Structures
- Use `moodycamel::ConcurrentQueue` for MPMC queues
- Avoid `std::mutex` on hot paths
- Use `std::atomic` for simple flags and counters

### Threading Best Practices
- Pin actors/tasks to specific threads (thread affinity)
- Minimize context switches
- Use dedicated thread pools for different workloads (e.g., I/O vs compute)

### Memory Management
- Use `std::unique_ptr` for ownership
- Use `std::shared_ptr` for shared ownership
- Avoid raw `new`/`delete`
- Use `std::make_unique` / `std::make_shared`

---

## Summary Checklist

When starting a new C++ project using this workflow:

- [ ] Create `claude/build.bat` with Visual Studio environment setup
- [ ] Create `CMakeLists.txt` with C++17, MSVC flags
- [ ] Set up project structure (include/, source/, tests/)
- [ ] Download Catch2 to `include/external/catch.hpp`
- [ ] Create `tests/test_common.h` with Catch2 main
- [ ] Create `CLAUDE.md` with project-specific instructions
- [ ] Create `.gitignore` with build artifacts
- [ ] Verify build: `./claude/build.bat` succeeds
- [ ] Write first test and verify it runs
- [ ] Document coding standards in project
- [ ] Set up CLion project (open folder, let it configure)

This workflow ensures consistent, professional C++ development with proper tooling, testing, and standards.
