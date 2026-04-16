# Actor Registry

The actor registry provides DNS-like name-based actor lookup, allowing actors to discover and communicate with each other without hard-coded references.

## Quick Example

```cpp
class worker : public cas::actor {
protected:
    void on_start() override {
        // Register this actor with a name
        set_name("worker_1");

        handler<task>(&worker::on_task);
    }

    void on_task(const task& msg) {
        // Process task...

        // Lookup manager actor by name
        auto manager = cas::actor_registry::get("manager");
        if (manager.is_valid()) {
            result_msg result;
            result.data = "task complete";
            manager.tell(result);
        }
    }
};

class manager : public cas::actor {
protected:
    void on_start() override {
        set_name("manager");  // Register as "manager"
        handler<result_msg>(&manager::on_result);
    }

    void on_result(const result_msg& msg) {
        std::cout << "Worker reported: " << msg.data << std::endl;
    }
};

int main() {
    auto mgr = cas::system::create<manager>();
    auto w1 = cas::system::create<worker>();

    cas::system::start();

    // Workers can find manager by name
    task t;
    w1.tell(t);

    cas::system::shutdown();
    cas::system::wait_for_shutdown();
}
```

## Registration API

### set_name()

Register an actor with a name in `on_start()`:

```cpp
class my_actor : public cas::actor {
protected:
    void on_start() override {
        set_name("my_unique_name");
        // Actor is now registered and can be looked up
    }
};
```

**Rules:**
- Must be called in `on_start()` (not constructor)
- Name should be unique (duplicate names: last one wins)
- Name persists until actor stops
- Actor automatically unregistered on stop

### Auto-Generated Names

If you don't call `set_name()`, actors get auto-generated names:

```cpp
class worker : public cas::actor {
protected:
    void on_start() override {
        // No set_name() called
        handler<task>(&worker::on_task);
    }
};

auto w = cas::system::create<worker>();
cas::system::start();

std::cout << w.name() << std::endl;
// Output: "worker_1" (typename_instanceId format)
```

**Auto-generated format:** `typename_instanceId`
- `typename` - Class name (e.g., "worker", "manager")
- `instanceId` - Unique sequential ID (1, 2, 3, ...)

### Custom Names with Parameters

Pass names via constructor:

```cpp
class named_worker : public cas::actor {
private:
    std::string m_name;

public:
    named_worker(const std::string& name) : m_name(name) {}

protected:
    void on_start() override {
        set_name(m_name);
        handler<task>(&named_worker::on_task);
    }
};

// Create workers with custom names
auto w1 = cas::system::create<named_worker>("worker_alpha");
auto w2 = cas::system::create<named_worker>("worker_beta");
auto w3 = cas::system::create<named_worker>("worker_gamma");
```

## Lookup API

### actor_registry::get()

Lookup an actor by name:

```cpp
cas::actor_ref cas::actor_registry::get(const std::string& name);
```

**Returns:**
- Valid `actor_ref` if actor found
- Invalid `actor_ref` if not found

**Example:**
```cpp
void on_start() override {
    handler<request>(&my_actor::on_request);

    // Lookup database actor
    auto db = cas::actor_registry::get("database");
    if (db.is_valid()) {
        std::cout << "Database actor found!" << std::endl;
    } else {
        std::cerr << "Database actor not found!" << std::endl;
    }
}
```

### actor_registry::exists()

Check if an actor name is registered:

```cpp
bool cas::actor_registry::exists(const std::string& name);
```

**Example:**
```cpp
if (cas::actor_registry::exists("cache")) {
    auto cache = cas::actor_registry::get("cache");
    cache.tell(msg);
} else {
    std::cerr << "Cache not available" << std::endl;
}
```

### actor_registry::count()

Get total number of registered actors:

```cpp
size_t cas::actor_registry::count();
```

**Example:**
```cpp
void on_start() override {
    size_t actor_count = cas::actor_registry::count();
    std::cout << "Registry contains " << actor_count << " actors" << std::endl;
}
```

## Lifecycle and Timing

### Registration Timing

```
┌─────────────────┐
│ Actor Created   │  set_name() NOT yet effective
│                 │  Registry lookup FAILS
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ system::start() │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ on_start()      │  set_name() called here
│ called          │  Actor registered NOW
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ Registry Active │  Lookups succeed
│                 │  Actors can find each other
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ Actor stops     │  Actor auto-unregistered
└─────────────────┘
```

**Key points:**
- Registry only populated **after** `system::start()`
- All `on_start()` hooks run **before** any messages processed
- Safe to lookup actors in `on_start()` (all registered by then)

### Lookup Before Start

```cpp
int main() {
    auto actor = cas::system::create<my_actor>();

    // ✗ BAD: Lookup before start
    auto ref = cas::actor_registry::get("my_actor");
    // Returns INVALID - on_start() hasn't run yet!

    cas::system::start();

    // ✓ GOOD: Lookup after start
    ref = cas::actor_registry::get("my_actor");
    // Returns VALID - on_start() has completed
}
```

### Automatic Unregistration

Actors are automatically removed from registry when stopped:

```cpp
auto actor = cas::system::create<my_actor>();
cas::system::start();

// Actor registered
REQUIRE(cas::actor_registry::exists("my_actor") == true);

// Stop actor
cas::system::stop_actor(actor);

// Actor unregistered
REQUIRE(cas::actor_registry::exists("my_actor") == false);
```

## Common Patterns

### Service Locator Pattern

Central actor that provides references to other actors:

```cpp
class service_locator : public cas::actor {
private:
    std::map<std::string, cas::actor_ref> m_services;

protected:
    void on_start() override {
        set_name("service_locator");

        handler<register_service>(&service_locator::on_register);
        handler<get_service>(&service_locator::on_get);

        // Register well-known services
        m_services["database"] = cas::actor_registry::get("database");
        m_services["cache"] = cas::actor_registry::get("cache");
        m_services["logger"] = cas::actor_registry::get("logger");
    }

    void on_register(const register_service& msg) {
        m_services[msg.name] = msg.sender;
    }

    void on_get(const get_service& msg) {
        if (msg.sender.is_valid()) {
            service_response resp;
            resp.name = msg.service_name;

            auto it = m_services.find(msg.service_name);
            if (it != m_services.end()) {
                resp.service = it->second;
            }

            msg.sender.tell(resp);
        }
    }
};
```

### Worker Pool Pattern

Named workers for load balancing:

```cpp
class worker_pool_manager : public cas::actor {
private:
    std::vector<cas::actor_ref> m_workers;
    size_t m_next_worker = 0;

protected:
    void on_start() override {
        set_name("pool_manager");
        handler<task>(&worker_pool_manager::on_task);

        // Lookup workers by name pattern
        for (int i = 1; i <= 5; ++i) {
            std::string name = "worker_" + std::to_string(i);
            auto worker = cas::actor_registry::get(name);
            if (worker.is_valid()) {
                m_workers.push_back(worker);
            }
        }

        std::cout << "Found " << m_workers.size() << " workers" << std::endl;
    }

    void on_task(const task& msg) {
        if (!m_workers.empty()) {
            // Round-robin distribution
            m_workers[m_next_worker].tell(msg);
            m_next_worker = (m_next_worker + 1) % m_workers.size();
        }
    }
};

// Create workers with predictable names
for (int i = 1; i <= 5; ++i) {
    auto worker = cas::system::create<worker_actor>(
        "worker_" + std::to_string(i)
    );
}
```

### Dependency Discovery Pattern

Actors discover dependencies at startup:

```cpp
class api_handler : public cas::actor {
private:
    cas::actor_ref m_database;
    cas::actor_ref m_cache;
    cas::actor_ref m_logger;

    bool m_ready = false;

protected:
    void on_start() override {
        set_name("api_handler");
        handler<http_request>(&api_handler::on_request);

        // Discover dependencies
        m_database = cas::actor_registry::get("database");
        m_cache = cas::actor_registry::get("cache");
        m_logger = cas::actor_registry::get("logger");

        // Verify critical dependencies
        if (!m_database.is_valid()) {
            std::cerr << "FATAL: Database actor not found!" << std::endl;
            return;  // Not ready
        }

        // Optional dependencies
        if (!m_cache.is_valid()) {
            std::cerr << "Warning: Cache actor not found, "
                      << "performance may be degraded" << std::endl;
        }

        m_ready = true;

        if (m_logger.is_valid()) {
            log_msg msg;
            msg.text = "API handler initialized";
            m_logger.tell(msg);
        }
    }

    void on_request(const http_request& msg) {
        if (!m_ready) {
            // Return error - dependencies not available
            return;
        }

        // Use dependencies...
    }
};
```

### Hierarchical Naming

Use naming conventions for organization:

```cpp
// Market data actors
cas::system::create<orderbook_actor>("market.orderbook.ES");
cas::system::create<orderbook_actor>("market.orderbook.NQ");
cas::system::create<orderbook_actor>("market.orderbook.YM");

// Account actors
cas::system::create<account_actor>("account.user1");
cas::system::create<account_actor>("account.user2");

// Risk management actors
cas::system::create<risk_actor>("risk.monitor");
cas::system::create<risk_actor>("risk.limits");

// Lookup by category prefix
std::vector<std::string> market_actors = {
    "market.orderbook.ES",
    "market.orderbook.NQ",
    "market.orderbook.YM"
};

for (const auto& name : market_actors) {
    auto actor = cas::actor_registry::get(name);
    if (actor.is_valid()) {
        actor.tell(shutdown_msg{});
    }
}
```

### Late Binding Pattern

Defer actor lookup until first use:

```cpp
class late_binding_actor : public cas::actor {
private:
    cas::actor_ref m_logger;
    bool m_logger_resolved = false;

protected:
    void on_start() override {
        handler<work>(&late_binding_actor::on_work);
        // Don't lookup logger here
    }

    void on_work(const work& msg) {
        // Lazy lookup on first use
        if (!m_logger_resolved) {
            m_logger = cas::actor_registry::get("logger");
            m_logger_resolved = true;

            if (!m_logger.is_valid()) {
                std::cerr << "Logger not available" << std::endl;
            }
        }

        // Use logger if available
        if (m_logger.is_valid()) {
            log_msg log;
            log.text = "Processing work";
            m_logger.tell(log);
        }

        // Process work...
    }
};
```

## Best Practices

### DO: Set Names in on_start()

```cpp
class my_actor : public cas::actor {
protected:
    void on_start() override {
        // ✓ GOOD: Set name here
        set_name("my_service");
    }
};

// ✗ BAD: Don't set in constructor
class bad_actor : public cas::actor {
public:
    bad_actor() {
        // System not ready - this won't work
        // set_name("bad");  // Don't do this!
    }
};
```

### DO: Use Meaningful Names

```cpp
// ✓ GOOD: Descriptive, hierarchical names
set_name("database.primary");
set_name("api.handler.v1");
set_name("orderbook.ES.futures");

// ✗ BAD: Generic or unclear names
set_name("actor1");
set_name("thing");
set_name("temp");
```

### DO: Check Validity After Lookup

```cpp
void on_start() override {
    auto db = cas::actor_registry::get("database");

    // ✓ GOOD: Check before using
    if (db.is_valid()) {
        db.tell(msg);
    } else {
        std::cerr << "Database not available" << std::endl;
        // Handle missing dependency
    }

    // ✗ BAD: Assume it exists
    // db.tell(msg);  // May fail if invalid!
}
```

### DO: Handle Missing Dependencies Gracefully

```cpp
void on_start() override {
    auto cache = cas::actor_registry::get("cache");

    if (cache.is_valid()) {
        m_cache = cache;
        std::cout << "Cache available" << std::endl;
    } else {
        // ✓ GOOD: Graceful degradation
        std::cout << "Cache unavailable, using direct DB access" << std::endl;
        m_cache_enabled = false;
    }
}
```

### DON'T: Lookup Before system::start()

```cpp
int main() {
    auto actor = cas::system::create<my_actor>();

    // ✗ BAD: Lookup before start
    auto ref = cas::actor_registry::get("my_actor");  // INVALID!

    cas::system::start();

    // ✓ GOOD: Lookup after start
    ref = cas::actor_registry::get("my_actor");  // VALID
}
```

### DON'T: Rely on Specific Instance IDs

```cpp
// ✗ BAD: Hardcoding instance IDs
auto worker = cas::actor_registry::get("worker_1");  // Might not exist

// ✓ GOOD: Use meaningful custom names
auto worker = cas::actor_registry::get("primary_worker");

// Or better: Use dependency injection
class manager : public cas::actor {
private:
    cas::actor_ref m_worker;

public:
    manager(cas::actor_ref worker) : m_worker(worker) {}

    void on_start() override {
        // Worker reference already provided
        m_worker.tell(task{});
    }
};
```

### DON'T: Use Same Name for Multiple Actors

```cpp
// ✗ BAD: Duplicate names (last one wins)
auto w1 = cas::system::create<worker>();  // set_name("worker")
auto w2 = cas::system::create<worker>();  // set_name("worker") - overwrites w1!

cas::system::start();

auto w = cas::actor_registry::get("worker");  // Returns w2, not w1!

// ✓ GOOD: Unique names
class worker : public cas::actor {
private:
    std::string m_name;

public:
    worker(const std::string& name) : m_name(name) {}

    void on_start() override {
        set_name(m_name);
    }
};

auto w1 = cas::system::create<worker>("worker_1");
auto w2 = cas::system::create<worker>("worker_2");
```

## Naming Conventions

### Recommendation: Hierarchical Names

Use dot-separated hierarchical names:

```
<system>.<component>.<instance>

Examples:
- trading.orderbook.ES
- trading.orderbook.NQ
- trading.risk.monitor
- api.http.handler
- database.cache.primary
- database.cache.secondary
```

### Pattern: Environment-Specific Names

```cpp
#ifdef PRODUCTION
    set_name("database.production.primary");
#else
    set_name("database.dev.primary");
#endif
```

### Pattern: Version-Specific Names

```cpp
set_name("api.handler.v2");  // Version 2 API
set_name("api.handler.v1");  // Legacy version 1
```

## Performance Considerations

### Lookup Cost

Registry lookup is fast but not free:

- Lookup time: O(log n) - uses hash map
- Memory: ~64 bytes per registered actor
- Thread-safe: Uses mutex for registry access

**Recommendation:** Lookup once in `on_start()`, store reference:

```cpp
class my_actor : public cas::actor {
private:
    cas::actor_ref m_database;  // ✓ Store reference

protected:
    void on_start() override {
        // ✓ GOOD: Lookup once, store
        m_database = cas::actor_registry::get("database");
    }

    void on_message(const msg& m) {
        // ✓ GOOD: Reuse stored reference
        m_database.tell(query{});

        // ✗ BAD: Repeated lookups
        // auto db = cas::actor_registry::get("database");  // Unnecessary!
        // db.tell(query{});
    }
};
```

### Registry Size

Registry scales well:
- Hundreds of actors: No performance impact
- Thousands of actors: Minimal impact
- Tens of thousands: Still acceptable

## Debugging

### List All Registered Actors

The registry doesn't provide iteration, but you can track actors yourself:

```cpp
class registry_monitor : public cas::actor {
private:
    std::vector<std::string> m_known_names;

protected:
    void on_start() override {
        // Known actor names
        m_known_names = {
            "database", "cache", "logger",
            "api_handler", "worker_1", "worker_2"
        };

        // Check which are registered
        for (const auto& name : m_known_names) {
            if (cas::actor_registry::exists(name)) {
                std::cout << "✓ " << name << " registered" << std::endl;
            } else {
                std::cout << "✗ " << name << " missing" << std::endl;
            }
        }

        std::cout << "Total registered: "
                  << cas::actor_registry::count() << std::endl;
    }
};
```

### Verify Dependencies at Startup

```cpp
void on_start() override {
    std::vector<std::string> required_deps = {
        "database", "logger"
    };

    std::vector<std::string> optional_deps = {
        "cache", "metrics"
    };

    bool ready = true;

    for (const auto& dep : required_deps) {
        if (!cas::actor_registry::exists(dep)) {
            std::cerr << "FATAL: Missing required dependency: "
                      << dep << std::endl;
            ready = false;
        }
    }

    for (const auto& dep : optional_deps) {
        if (!cas::actor_registry::exists(dep)) {
            std::cout << "Warning: Missing optional dependency: "
                      << dep << std::endl;
        }
    }

    if (ready) {
        std::cout << "All dependencies satisfied" << std::endl;
    }
}
```

## Comparison with Other Patterns

| Pattern | Pros | Cons |
|---------|------|------|
| **Registry** | Decoupled, flexible, runtime lookup | String-based (no compile-time check) |
| **Constructor Injection** | Type-safe, explicit dependencies | Tight coupling, verbose |
| **Global Variables** | Simple, fast | Unsafe, hard to test, global state |

**Recommendation:** Use registry for loose coupling, constructor injection for critical type-safe dependencies.

## Next Steps

- [Message Passing](40_message_passing.md) - Send messages to registered actors
- [Lifecycle Hooks](50_lifecycle_hooks.md) - When to lookup actors
- [Dynamic Removal](70_dynamic_removal.md) - Automatic unregistration on stop
- [Best Practices](120_best_practices.md) - Design patterns and guidelines
