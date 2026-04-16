# Cygnus Actor Framework Documentation

Complete documentation for the Cygnus Actor Framework - a modern C++17 actor framework for high-performance concurrent applications.

## Documentation Index

Documentation files are numbered for easy ordering and insertion of new topics.

### Getting Started
- **[10_overview.md](10_overview.md)** - Framework overview, architecture, and core concepts
- **[20_getting_started.md](20_getting_started.md)** - Quick start guide with complete examples

### Core Features
- **30_creating_actors.md** - Actor creation patterns and best practices *(coming soon)*
- **[40_message_passing.md](40_message_passing.md)** - Message definition, handlers, and sending patterns
- **[50_lifecycle_hooks.md](50_lifecycle_hooks.md)** - on_start(), on_shutdown(), on_stop() usage
- **60_actor_registry.md** - Name-based actor lookup and discovery *(coming soon)*

### Dynamic Management
- **[70_dynamic_removal.md](70_dynamic_removal.md)** - Stop actors at runtime, watch pattern, lifecycle management

### Advanced Features
- **[80_ask_pattern.md](80_ask_pattern.md)** - RPC-style synchronous messaging with timeouts
- **[90_timers.md](90_timers.md)** - One-shot and periodic timers
- **100_advanced_actors.md** - Fast actors, inline actors, stateful actors *(coming soon)*

### Configuration & Best Practices
- **110_configuration.md** - System configuration options *(coming soon)*
- **120_best_practices.md** - Design patterns, anti-patterns, performance tips *(coming soon)*

### Inter-Process Communication
- **[130_zeromq_relay.md](130_zeromq_relay.md)** - ZeroMQ relay actor for inter-process communication

## Quick Links

### Common Tasks
- [Create your first actor](20_getting_started.md#step-3-create-your-actor)
- [Send messages between actors](20_getting_started.md#actor-to-actor-communication)
- [Define and handle messages](40_message_passing.md#message-definition)
- [Initialize actors properly](50_lifecycle_hooks.md#on_start)
- [Schedule delayed messages](90_timers.md#one-shot-timers)
- [Stop an actor at runtime](70_dynamic_removal.md#quick-example)
- [Make synchronous RPC calls](80_ask_pattern.md#quick-example)
- [Handle actor termination](70_dynamic_removal.md#watch-pattern)
- [Inter-process communication](130_zeromq_relay.md#quick-example)

### API Reference
- [System API](10_overview.md#core-concepts) - `cas::system`
- [Actor API](20_getting_started.md#step-3-create-your-actor) - `cas::actor`
- [Actor Reference API](20_getting_started.md#actor-references) - `cas::actor_ref`
- [Message API](20_getting_started.md#step-2-define-your-messages) - `cas::message_base`

## Feature Overview

### Core Actor System
- Actor-based concurrency (no locks, no shared state)
- Type-safe message passing
- Lock-free message queues (moodycamel::ConcurrentQueue)
- Thread affinity (actors pinned to threads)
- Actor registry (name-based lookup)
- Graceful shutdown with message draining
- **NEW:** Dynamic actor creation and removal at runtime

### Message Patterns
- Asynchronous fire-and-forget messaging
- Request-response with sender tracking
- **Ask pattern** for synchronous RPC-style calls
- Ask with timeout support
- Message correlation IDs

### Actor Lifecycle
- `on_start()` - Initialization and handler registration
- `on_shutdown()` - Pre-shutdown cleanup (can send messages)
- `on_stop()` - Final cleanup (no message sending)
- Automatic timer cancellation on stop
- **Watch pattern** for termination notifications

### Advanced Actor Types
- **Fast Actors** - Dedicated threads, <1μs latency
- **Inline Actors** - Zero-latency synchronous execution
- **Stateful Actors** - State-based message filtering

### Timing & Scheduling
- One-shot timers (`schedule_once`)
- Periodic timers (`schedule_periodic`)
- Timer cancellation
- Automatic cleanup on actor stop

### Thread Models
- Pooled actors (default) - Thread pool with affinity
- Fast actors - One dedicated thread per actor
- Inline actors - Execute in caller's thread
- Ask pattern - Dedicated RPC thread pool

### Inter-Process Communication
- **ZeroMQ relay actor** - ROUTER/DEALER patterns for cross-process messaging
- TCP, IPC, and inproc transports
- Generic payload (FIX, JSON, binary, etc.)

## Examples

All documentation includes complete, runnable examples. Additional examples can be found in:
- `tests/example_usage.cpp` - Complete ping-pong example
- `tests/00_basic/` - Basic usage patterns
- `tests/02_interactions/` - Actor communication
- `tests/05_ask_pattern/` - RPC-style messaging
- `tests/06_timers/` - Timer usage
- `tests/07_dynamic_removal/` - Dynamic actor management
- `tests/08_zeromq_relay/` - ZeroMQ inter-process communication

## Coding Conventions

Cygnus follows strict naming conventions:
- **All identifiers**: `snake_case` (NO camelCase)
- **Member variables**: `m_` prefix
- **Struct members**: No prefix
- **Accessors**: Property-style (no `get_`/`set_`)

See [Overview - Naming Conventions](10_overview.md#naming-conventions) for details.

## Platform Support

- **Windows**: MSVC 2017+, Visual Studio 2022 recommended
- **Linux**: GCC 7+, Clang 5+
- **macOS**: Clang 5+ (Xcode 9.3+)

Requires C++17 or later.

## Building

```bash
# Configure
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build

# Run tests
./build/unit_tests
```

See [Getting Started - Building](20_getting_started.md#building-cygnus) for platform-specific instructions.

## Contributing

When adding new features or documentation:

1. **Documentation files** - Use numbered prefixes (e.g., `85_new_feature.md`)
2. **Insertion points** - Numbers allow easy insertion between existing docs
3. **Update this README** - Add new docs to the index above
4. **Complete examples** - All docs should include runnable code
5. **Cross-reference** - Link to related documentation

## Version History

### Version 0.2.0 (Current)
- ✅ Dynamic actor creation and removal
- ✅ Watch pattern for termination notifications
- ✅ Synchronous and asynchronous stop modes
- ✅ Drain and discard message handling
- ✅ ZeroMQ relay actor for inter-process communication

### Version 0.1.0
- ✅ Core actor system
- ✅ Message passing with sender tracking
- ✅ Actor registry
- ✅ Lifecycle hooks
- ✅ Ask pattern (RPC)
- ✅ Timers (one-shot and periodic)
- ✅ Fast actors
- ✅ Inline actors
- ✅ Stateful actors

## License

[Add license information here]

## Support

- **Issues**: [GitHub Issues](https://github.com/your-repo/cygnus/issues)
- **Discussions**: [GitHub Discussions](https://github.com/your-repo/cygnus/discussions)
- **Email**: your-email@example.com

## Next Steps

1. Start with [Overview](10_overview.md) to understand core concepts
2. Follow [Getting Started](20_getting_started.md) to build your first application
3. Explore [Dynamic Removal](70_dynamic_removal.md) for runtime actor management
4. Learn [Ask Pattern](80_ask_pattern.md) for synchronous operations
5. Use [ZeroMQ Relay](130_zeromq_relay.md) for inter-process communication
