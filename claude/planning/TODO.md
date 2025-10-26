# Cygnus Actor Framework - TODO

**Last updated**: 2025-10-26

## Current Session TODOs

### Completed ✅
- [x] Add actor identity (instance_id, type_name)
- [x] Implement auto-generated actor names (typename_id)
- [x] Fix message sender bug (already fixed via thread-local context)
- [x] Add queue metrics (high water marks)
- [x] Add configurable queue threshold warnings
- [x] Reorganize planning documents
- [x] Add comprehensive test suite for ask pattern (9 tests, 19 assertions)
- [x] Fix ask pattern compilation (friend template, include headers)

### In Progress 🔄
- None currently

### Next Up ⏭️
- None currently

## Deferred / Blocked 🚫
- **stateful_actor**: Disabled due to ABI sensitivity issues with base class layout
  - Will revisit after core features stabilized
  - See: `planning/automatic_handler_registration.md` for potential solution

## Future Features 🔮

See `claude/planning/future_features_roadmap.md` for comprehensive roadmap.

### Phase 1: Core Stability (Pre-1.0)
1. **Supervision & Fault Tolerance** (4-6 weeks)
2. **Ask Pattern** (2-3 weeks)
3. **Bounded Mailboxes** (1-2 weeks)
4. **Timers** (2-3 weeks)

### Phase 2: Advanced Features (Post-1.0)
5. **Extended Lifecycle Events** (1-2 weeks)
6. **Message Patterns** (2-3 weeks)
7. **Metrics & Observability** (2-3 weeks)
8. **Testing Framework** (2-3 weeks)

### Phase 3: Distribution (Future)
9. **Serialization Framework** (4-6 weeks)
10. **Remote Actors with ZeroMQ** (8-12 weeks)
11. **Cluster Support** (6-8 weeks)

## Technical Debt 🧹

### Code Quality
- [ ] Replace std::queue + mutex with lock-free queues (moodycamel::ConcurrentQueue)
- [ ] Add condition variables for efficient thread waking (replace sleep polling)
- [ ] Message pooling/reuse to reduce allocations

### Documentation
- [ ] Add Doxygen comments to public APIs
- [ ] Create user guide with examples
- [ ] Write architecture decision records (ADRs)

### Testing
- [ ] Increase test coverage for edge cases
- [ ] Add performance benchmarks
- [ ] Add stress tests for multi-threaded scenarios

## Notes 📝

### Important Decisions
- **IPC/Remote**: Use ZeroMQ (don't reinvent the wheel)
- **Naming**: snake_case for everything
- **Dependencies**: Prefer battle-tested open source libraries with permissive licenses

### Open Questions
- Should we support dynamic supervision trees, or only static hierarchies?
- Ask pattern: std::future, custom future, or callback-based?
- Serialization: Mandatory or optional? Code generation or reflection?
- Memory model: Shared-nothing strict, or allow shared state with locks?
