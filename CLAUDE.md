# Claude Code Notes for CYGNUS_ACTOR_FRAMEWORK

## Quick Reference

### Building the Project
```bash
# Easiest method - use the build script:
./claude/build.bat

# Or build in CLion (Ctrl+F9)
```

### Running Tests
```bash
# After building:
./cmake-build-debug/example_usage.exe
```

### Claude Working Directory
- **`claude/`** - All Claude-specific files, scripts, and documentation
  - `build.bat` - Build script that sets up Visual Studio environment
  - `planning/` - Design docs, bug reports, research notes
    - `bug_message_sender.md` - Critical bug documentation
    - `build_setup_paths.md` - Build system setup guide
    - `CLAUDE.md` - Framework design notes and principles
    - Other planning documents...

## Build System Information

### Quick Build (Recommended)
Use the provided build script that automatically sets up the Visual Studio environment:

```bash
./claude/build.bat
```

This script:
1. Calls `vcvarsall.bat` to set up INCLUDE, LIB, and PATH
2. Runs CMake build
3. Reports success/failure

### Manual Build Options

**Option 1: Using Developer Command Prompt**
```cmd
# Open "Developer Command Prompt for VS 2022" from Start Menu, then:
cd C:\Users\ritte\_PR_DEV_\DEV_CPP\CYGNUS_ACTOR_FRAMEWORK
cmake --build cmake-build-debug --target example_usage
```

**Option 2: Build in CLion**
- Use Build > Build 'example_usage' or `Ctrl+F9`
- CLion automatically configures the environment

### Build System Details

**Visual Studio Setup:**
- Base Path: `C:\Program Files\Microsoft Visual Studio\2022\Community`
- MSVC Version: 14.44.35207
- Compiler: cl.exe
- Build Tool: nmake.exe
- CMake: `C:\Users\ritte\AppData\Local\Programs\CLion 2\bin\cmake\win\x64\bin\cmake.exe`

**Environment Requirements:**
Building from command line requires these environment variables:
- `PATH` - Must include MSVC tools (cl.exe, nmake.exe, link.exe)
- `INCLUDE` - C++ standard library and Windows SDK headers
- `LIB` - Standard library and Windows SDK libraries

The `vcvarsall.bat` script sets up all of these automatically. See `claude/planning/build_setup_paths.md` for details on permanent PATH setup.

### Running the Executable

**Direct execution:**
```bash
./cmake-build-debug/example_usage.exe
```

**Important**: Always use timeout when running in Claude Code since programs may hang:
- Use `timeout` parameter in Bash tool (milliseconds)
- Use `run_in_background` + `BashOutput` for monitoring
- Use `KillShell` to terminate hung processes

**Critical**: Remove all breakpoints before running from command line!
- Debug builds contain breakpoint instructions
- Running without debugger attached will cause program to hang at breakpoints
- Either remove breakpoints or switch to Release build

## Project Structure

```
CYGNUS_ACTOR_FRAMEWORK/
├── claude/                  # Claude working directory
│   ├── build.bat           # Build script (sets up VS environment)
│   └── planning/           # Design docs and planning
│       ├── CLAUDE.md       # Framework design principles
│       ├── bug_message_sender.md
│       ├── build_setup_paths.md
│       └── ...
├── include/cas/            # Framework headers
│   ├── actor.h            # Base actor class
│   ├── actor_ref.h        # Actor reference/handle
│   ├── message_base.h     # Base message struct
│   ├── actor_registry.h   # Name-based actor lookup
│   ├── system.h           # Actor system runtime
│   └── cas.h              # Main include (includes all above)
├── source/                # Implementation files
│   ├── actor.cpp
│   ├── actor_ref.cpp
│   ├── actor_registry.cpp
│   └── system.cpp
├── tests/
│   └── example_usage.cpp  # Example demonstrating framework
├── planning/              # User planning directory (preserved)
└── cmake-build-debug/     # Build output
    ├── CygnusActorFramework.lib
    └── example_usage.exe
```

## Known Issues

### Critical Bug: Message Sender
- **Status**: 🔴 CRITICAL - Blocks all reply-based communication
- **Problem**: `msg.sender` is set to target actor instead of source actor
- **Impact**: Replies are sent back to the wrong actor, causing hangs
- **Details**: See `claude/planning/bug_message_sender.md`
- **Symptom**: Ping-pong example hangs after first exchange

### Build System
- **Status**: ✅ RESOLVED - Use `claude/build.bat`
- Building requires full Visual Studio environment (INCLUDE, LIB, PATH)
- The build.bat script handles this automatically
- See `claude/planning/build_setup_paths.md` for permanent PATH setup

### Running Programs
- **Issue**: Programs may hang indefinitely (especially with current sender bug)
- **Solution**: Always use timeout (5000-10000ms recommended)
- **Tools**: BashOutput to monitor, KillShell to terminate

## Framework Status

### Features Implemented
- ✅ Actor base class with lifecycle hooks (`on_start`, `on_shutdown`, `on_stop`)
- ✅ Message passing with `actor_ref.receive(msg)`
- ✅ Handler registration: `handler<MessageType>(&Actor::method)` and lambda style
- ✅ Thread affinity (each actor pinned to one thread)
- ✅ Two-queue priority system (regular mailbox + ask queue)
- ✅ Graceful shutdown with message draining
- ✅ Actor state tracking (running/stopping/stopped)
- ✅ Actor registry for name-based lookup
- 🔴 **BROKEN**: Message sender tracking (critical bug)
- ⏳ RPC-style `invoke()` (registered but not implemented yet)

### API Examples

**Creating actors:**
```cpp
auto pong = cas::system::create<actor::pong>();
auto ping = cas::system::create<actor::ping>();
```

**Starting system:**
```cpp
cas::system::start();
```

**Shutdown:**
```cpp
cas::system::shutdown();           // Initiate graceful shutdown
cas::system::wait_for_shutdown();  // Wait for completion
```

**Message handler registration:**
```cpp
// Method pointer style
handler<message::ping>(&pong::on_ping);

// Lambda style
handler<message::pong>([this](const message::pong& msg) {
    on_pong(msg);
});
```

**Sending messages:**
```cpp
actor_ref.receive(msg);  // Primary API
actor_ref << msg;        // Operator overload
```

## Coding Standard

### Naming Conventions
- **All identifiers**: Use `snake_case` (NO camelCase)
- **Member variables**: Use `m_` prefix without trailing underscore
  - Example: `m_name`, `m_thread_id`, `m_running`
- **Struct public members**: NO `m_` prefix (plain names)
  - Example: In `message_base`: `sender`, not `m_sender`
- **Accessor methods**: Use property-style naming, NOT `get_`/`set_` prefixes
  - ✅ Good: `name()` and `name(const std::string&)`
  - ❌ Bad: `get_name()` and `set_name()`
- **Exception**: Use `get` ONLY for smart pointer-like operations (matching standard library)
  - Example: `actor_ref::get<T>()`, `actor_ref::get_checked<T>()`

### Code Organization
- **Function definitions**: Implement in `.cpp` files, even if small
  - Compilers are smart enough to inline what they need
  - Exception: Template functions must stay in headers
- **Documentation**: Use Doxygen style (`///` or `/** @brief */`) in headers ONLY
  - Document public API in header files
  - No documentation needed in `.cpp` implementation files

### API Design Philosophy
- **Match standard library conventions** wherever possible
- **Avoid exposing raw pointers** in public APIs
- Use `actor_ref` as the primary user-facing handle type
- Prefer references over pointers when ownership is not transferred

### Style Notes
- Struct vs Class:
  - Use `struct` for simple data holders (like messages)
  - Use `class` for types with behavior and invariants (like `actor`, `system`)

## Documentation

For detailed design notes, see `claude/planning/CLAUDE.md`:
- API design philosophy
- Threading model
- Message dispatch design
- Actor lifecycle
- Dependencies strategy
- And more...
