# Ask Pattern (RPC-Style Messaging)

The ask pattern provides synchronous request-response messaging, similar to remote procedure calls (RPC). Unlike regular asynchronous messages, ask calls **block** until the result is ready, making them ideal for operations that need immediate results.

## Quick Example

```cpp
struct calculate_op {};  // Operation tag

class calculator : public cas::actor {
protected:
    void on_start() override {
        set_name("calculator");
        ask_handler<int, calculate_op>(&calculator::calculate);
    }

    int calculate(int a, int b) {
        return a + b;
    }
};

int main() {
    auto calc = cas::system::create<calculator>();
    cas::system::start();

    // Synchronous call - blocks until result ready
    int result = calc.ask<int>(calculate_op{}, 10, 5);
    std::cout << "10 + 5 = " << result << std::endl;  // 15

    cas::system::shutdown();
    cas::system::wait_for_shutdown();
}
```

## Basic Ask Pattern

### Register Ask Handler

```cpp
ask_handler<ReturnType, OperationTag>(&ActorClass::method);
```

**Components:**
- `ReturnType` - Type of value returned
- `OperationTag` - Empty struct identifying the operation
- `method` - Member function that handles the request

**Example:**
```cpp
struct add_op {};
struct multiply_op {};

class calculator : public cas::actor {
protected:
    void on_start() override {
        ask_handler<int, add_op>(&calculator::add);
        ask_handler<int, multiply_op>(&calculator::multiply);
        ask_handler<double, multiply_op>(&calculator::multiply_double);
    }

    int add(int a, int b) {
        return a + b;
    }

    int multiply(int a, int b) {
        return a * b;
    }

    double multiply_double(double a, double b) {
        return a * b;
    }
};
```

### Call with Ask

```cpp
ReturnType result = actor_ref.ask<ReturnType>(operation_tag, args...);
```

**Parameters:**
- `ReturnType` - Expected return type (must match handler)
- `operation_tag` - Instance of operation tag struct
- `args...` - Arguments forwarded to handler method

**Example:**
```cpp
auto calc = cas::system::create<calculator>();
cas::system::start();

// Call different operations
int sum = calc.ask<int>(add_op{}, 10, 5);
int product = calc.ask<int>(multiply_op{}, 10, 5);
double product_d = calc.ask<double>(multiply_op{}, 3.14, 2.0);

std::cout << "Sum: " << sum << std::endl;           // 15
std::cout << "Product: " << product << std::endl;    // 50
std::cout << "Product (double): " << product_d << std::endl;  // 6.28
```

## Ask with Timeout

For operations that might take too long, use timeouts:

```cpp
std::optional<ReturnType> result = actor_ref.ask<ReturnType>(
    operation_tag,
    timeout_duration,
    args...
);
```

**Returns:**
- `std::optional<ReturnType>` with value if completed
- `std::nullopt` if timeout occurred

**Example:**
```cpp
struct slow_op {};

class slow_actor : public cas::actor {
protected:
    void on_start() override {
        ask_handler<int, slow_op>(&slow_actor::slow_operation);
    }

    int slow_operation(int delay_ms, int value) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(delay_ms)
        );
        return value * 2;
    }
};

int main() {
    auto actor = cas::system::create<slow_actor>();
    cas::system::start();

    // Fast call - succeeds
    auto result1 = actor.ask<int>(
        slow_op{},
        std::chrono::milliseconds(500),  // timeout
        50,   // delay_ms argument
        42    // value argument
    );

    if (result1.has_value()) {
        std::cout << "Result: " << result1.value() << std::endl;  // 84
    }

    // Slow call - times out
    auto result2 = actor.ask<int>(
        slow_op{},
        std::chrono::milliseconds(100),  // timeout
        500,  // delay_ms argument (longer than timeout!)
        42    // value argument
    );

    if (!result2.has_value()) {
        std::cout << "Operation timed out" << std::endl;
    }

    cas::system::shutdown();
    cas::system::wait_for_shutdown();
}
```

## Multiple Operations per Actor

Actors can handle multiple ask operations:

```cpp
struct get_balance_op {};
struct withdraw_op {};
struct deposit_op {};

class account : public cas::actor {
private:
    double m_balance = 0.0;

protected:
    void on_start() override {
        set_name("account");

        ask_handler<double, get_balance_op>(&account::get_balance);
        ask_handler<bool, withdraw_op>(&account::withdraw);
        ask_handler<void, deposit_op>(&account::deposit);
    }

    double get_balance() {
        return m_balance;
    }

    bool withdraw(double amount) {
        if (m_balance >= amount) {
            m_balance -= amount;
            return true;  // Success
        }
        return false;  // Insufficient funds
    }

    void deposit(double amount) {
        m_balance += amount;
        // void return type
    }
};

int main() {
    auto acct = cas::system::create<account>();
    cas::system::start();

    // Deposit
    acct.ask<void>(deposit_op{}, 100.0);

    // Check balance
    double balance = acct.ask<double>(get_balance_op{});
    std::cout << "Balance: " << balance << std::endl;  // 100.0

    // Withdraw
    bool success = acct.ask<bool>(withdraw_op{}, 30.0);
    std::cout << "Withdraw: " << (success ? "OK" : "Failed") << std::endl;

    // Check new balance
    balance = acct.ask<double>(get_balance_op{});
    std::cout << "New balance: " << balance << std::endl;  // 70.0

    cas::system::shutdown();
    cas::system::wait_for_shutdown();
}
```

## Return Types

### Primitive Types
```cpp
ask_handler<int, op>(&actor::method);
ask_handler<double, op>(&actor::method);
ask_handler<bool, op>(&actor::method);
```

### Void Return
```cpp
ask_handler<void, op>(&actor::method);

// Call site
actor.ask<void>(op{}, args...);  // Returns void
```

### Custom Types
```cpp
struct result_data {
    int code;
    std::string message;
};

ask_handler<result_data, op>(&actor::method);

// Call site
result_data res = actor.ask<result_data>(op{}, args...);
```

### Containers
```cpp
ask_handler<std::vector<int>, op>(&actor::method);
ask_handler<std::map<std::string, double>, op>(&actor::method);
ask_handler<std::optional<std::string>, op>(&actor::method);
```

## Error Handling

### Missing Handler

If no handler is registered for an operation, `ask()` throws:

```cpp
try {
    int result = actor.ask<int>(undefined_op{});
} catch (const std::runtime_error& e) {
    std::cerr << "Error: " << e.what() << std::endl;
    // "No ask handler registered for operation"
}
```

### Handler Exceptions

Exceptions from handlers propagate to the caller:

```cpp
class validator : public cas::actor {
protected:
    void on_start() override {
        ask_handler<int, validate_op>(&validator::validate);
    }

    int validate(int value) {
        if (value < 0) {
            throw std::invalid_argument("Value must be positive");
        }
        return value * 2;
    }
};

// Call site
try {
    int result = validator_ref.ask<int>(validate_op{}, -5);
} catch (const std::invalid_argument& e) {
    std::cerr << "Validation failed: " << e.what() << std::endl;
}
```

## Actor-to-Actor Ask

Actors can ask other actors:

```cpp
class frontend : public cas::actor {
private:
    cas::actor_ref m_backend;

protected:
    void on_start() override {
        set_name("frontend");
        handler<request>(&frontend::on_request);

        m_backend = cas::actor_registry::get("backend");
    }

    void on_request(const request& msg) {
        // Ask backend synchronously
        auto result = m_backend.ask<std::string>(
            process_op{},
            msg.data
        );

        std::cout << "Backend returned: " << result << std::endl;

        // Can continue processing or reply to original sender
        if (msg.sender.is_valid()) {
            response resp;
            resp.data = result;
            msg.sender.tell(resp);
        }
    }
};
```

**Important:** Ask calls from within actors are safe because Cygnus uses a dedicated thread pool for ask processing, preventing deadlocks.

## Dedicated Thread Pool

Ask operations are processed by a dedicated thread pool, separate from regular message processing threads. This prevents deadlocks and ensures ask calls don't block regular message processing.

**Configuration:**
```cpp
cas::system_config config;
config.ask_thread_pool_size = 8;  // Default: 4

cas::system::configure(config);
cas::system::start();
```

**Benefits:**
- No deadlock when actor A asks actor B, and B asks A
- Regular message processing not blocked by long ask operations
- Concurrent ask operations processed in parallel

## Performance Considerations

### When to Use Ask

**Use ask for:**
- ✓ Operations needing immediate results
- ✓ Synchronous APIs (REST endpoints, CLI commands)
- ✓ Database queries
- ✓ Calculations with dependencies
- ✓ Validation that must complete before continuing

**Don't use ask for:**
- ✗ Fire-and-forget operations (use `tell()` instead)
- ✗ High-frequency operations (async messages are faster)
- ✗ Long-running operations without timeout
- ✗ When you can restructure to use callbacks

### Performance Comparison

```cpp
// Ask (synchronous) - ~10-50 microseconds overhead
int result = actor.ask<int>(op{}, arg);  // Blocks

// Regular message (asynchronous) - ~1-10 microseconds overhead
actor.tell(msg);  // Returns immediately
```

## Use Cases

### REST API Handler

```cpp
class api_handler : public cas::actor {
private:
    cas::actor_ref m_database;

protected:
    void on_start() override {
        m_database = cas::actor_registry::get("database");
    }

    std::string handle_get_request(const std::string& user_id) {
        // Synchronous database query
        auto user_data = m_database.ask<std::optional<user>>(
            query_user_op{},
            std::chrono::seconds(5),  // Timeout
            user_id
        );

        if (user_data.has_value()) {
            return serialize_to_json(user_data.value());
        } else {
            return "{\"error\": \"User not found or timeout\"}";
        }
    }
};
```

### Configuration Manager

```cpp
class config_manager : public cas::actor {
private:
    std::map<std::string, std::string> m_config;

protected:
    void on_start() override {
        set_name("config");
        load_from_file("config.ini");

        ask_handler<std::string, get_config_op>(&config_manager::get_value);
        ask_handler<void, set_config_op>(&config_manager::set_value);
    }

    std::string get_value(const std::string& key) {
        auto it = m_config.find(key);
        return it != m_config.end() ? it->second : "";
    }

    void set_value(const std::string& key, const std::string& value) {
        m_config[key] = value;
    }
};

// Usage
auto config = cas::actor_registry::get("config");
std::string db_host = config.ask<std::string>(get_config_op{}, "database.host");
```

## Best Practices

### DO: Use Timeouts for External Operations

```cpp
auto result = actor.ask<data>(
    fetch_op{},
    std::chrono::seconds(10),  // Generous timeout
    url
);

if (!result.has_value()) {
    handle_timeout();
}
```

### DO: Keep Ask Handlers Fast

```cpp
// ✓ GOOD: Fast computation
int calculate(int a, int b) {
    return a + b;
}

// ✗ BAD: Slow blocking operation
std::string fetch_from_network(const std::string& url) {
    // This blocks the ask thread pool!
    return http_get(url);  // Can take seconds
}
```

### DO: Use Meaningful Operation Tags

```cpp
// ✓ GOOD: Clear, descriptive names
struct get_user_profile_op {};
struct calculate_shipping_cost_op {};
struct validate_credit_card_op {};

// ✗ BAD: Generic names
struct op1 {};
struct operation {};
struct do_something {};
```

### DON'T: Overuse Ask

```cpp
// ✗ BAD: Using ask when async would work
for (int i = 0; i < 1000; ++i) {
    actor.ask<void>(process_op{}, i);  // Synchronous, slow!
}

// ✓ GOOD: Use async messages
for (int i = 0; i < 1000; ++i) {
    actor.tell(process_msg{i});  // Asynchronous, fast!
}
```

## Comparison: Ask vs Regular Messages

| Feature | Ask Pattern | Regular Messages |
|---------|------------|------------------|
| Execution | Synchronous (blocks) | Asynchronous (returns immediately) |
| Return value | Yes | No (use callbacks) |
| Overhead | ~10-50 μs | ~1-10 μs |
| Thread pool | Dedicated ask pool | Worker thread pool |
| Timeout support | Yes | No |
| Use case | Need immediate result | Fire-and-forget |

## Next Steps

- [Timers](90_timers.md) - Schedule delayed and periodic messages
- [Advanced Actors](100_advanced_actors.md) - Fast, inline, and stateful actors
- [Best Practices](120_best_practices.md) - Design patterns and guidelines
