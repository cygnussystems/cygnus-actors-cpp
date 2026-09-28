# Call (RPC-Style Messaging)

The call provides synchronous request-response messaging, similar to remote procedure calls (RPC). Unlike regular asynchronous messages, `call()` **blocks** until the result is ready, making them ideal for operations that need immediate results.

## Quick Example

> **Runnable version:** `tutorials/06_call.cpp` — run `tutorials 6`.
> It is compiled and executed by `ctest`, and also demonstrates timeouts,
> exceptions crossing the actor boundary, and the deadlock check firing.

```cpp
struct calculate_op {};  // Operation tag

class calculator : public cas::actor {
protected:
    void on_start() override {
        set_name("calculator");
        call_handler<int, calculate_op>(&calculator::calculate);
    }

    int calculate(int a, int b) {
        return a + b;
    }
};

int main() {
    auto calc = cas::system::create<calculator>();
    cas::system::start();

    // Synchronous call - blocks until result ready
    int result = calc.call<int>(calculate_op{}, 10, 5);
    std::cout << "10 + 5 = " << result << std::endl;  // 15

    cas::system::shutdown();
    cas::system::wait_for_shutdown();
}
```

## Basic Call

### Register Call Handler

```cpp
call_handler<ReturnType, OperationTag>(&ActorClass::method);
```

**Components:**
- `ReturnType` - Type of value returned
- `OperationTag` - Empty struct identifying the operation
- `method` - Member function that handles the request

The method must be a **non-const member function** of the actor
(`ReturnType (Actor::*)(Args...)`). There is no lambda overload, and a `const`
method does not bind.

**Example:**
```cpp
struct add_op {};
struct multiply_op {};
struct multiply_double_op {};  // one handler per tag - a second registration replaces the first

class calculator : public cas::actor {
protected:
    void on_start() override {
        call_handler<int, add_op>(&calculator::add);
        call_handler<int, multiply_op>(&calculator::multiply);
        call_handler<double, multiply_double_op>(&calculator::multiply_double);
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

### Making a Call

```cpp
ReturnType result = actor_ref.call<ReturnType>(operation_tag, args...);
```

**Parameters:**
- `ReturnType` - Expected return type (must match the handler's exactly)
- `operation_tag` - Instance of operation tag struct
- `args...` - Arguments copied into the request and passed to the handler method

**Argument types must match exactly.** Each argument is copied, and its decayed
type must equal the decayed type of the corresponding handler parameter. There is
no implicit conversion: pass `10.0`, not `10`, to a `double` parameter, and
`std::string{"x"}`, not `"x"`, to a `std::string` parameter. Lvalues and
`const T&` parameters are fine. A mismatch in argument types, arity or
`ReturnType` throws `cas::call_type_error` to the caller, naming both types.

**Example:**
```cpp
auto calc = cas::system::create<calculator>();
cas::system::start();

// Call different operations
int sum = calc.call<int>(add_op{}, 10, 5);
int product = calc.call<int>(multiply_op{}, 10, 5);
double product_d = calc.call<double>(multiply_double_op{}, 3.14, 2.0);

std::cout << "Sum: " << sum << std::endl;           // 15
std::cout << "Product: " << product << std::endl;    // 50
std::cout << "Product (double): " << product_d << std::endl;  // 6.28
```

## Call with Timeout

For operations that might take too long, use timeouts:

```cpp
std::optional<ReturnType> result = actor_ref.call<ReturnType>(
    operation_tag,
    timeout_duration,
    args...
);
```

**Returns:**
- `std::optional<ReturnType>` with value if completed
- `std::nullopt` if timeout occurred

The timeout only replaces *waiting too long* with `std::nullopt`. Everything
else is still an exception: handler exceptions, `cas::call_type_error`, the
missing-handler `std::runtime_error`, `cas::call_deadlock_error` and
`cas::on_start_violation`.

A timeout does **not** cancel the request. The handler still runs when the
actor gets to it, and its side effects happen even though the caller already
got `std::nullopt`.

**Example:**
```cpp
struct slow_op {};

class slow_actor : public cas::actor {
protected:
    void on_start() override {
        call_handler<int, slow_op>(&slow_actor::slow_operation);
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
    auto result1 = actor.call<int>(
        slow_op{},
        std::chrono::milliseconds(500),  // timeout
        50,   // delay_ms argument
        42    // value argument
    );

    if (result1.has_value()) {
        std::cout << "Result: " << result1.value() << std::endl;  // 84
    }

    // Slow call - times out
    auto result2 = actor.call<int>(
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

Actors can handle multiple call operations:

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

        call_handler<double, get_balance_op>(&account::get_balance);
        call_handler<bool, withdraw_op>(&account::withdraw);
        call_handler<void, deposit_op>(&account::deposit);
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
    acct.call<void>(deposit_op{}, 100.0);

    // Check balance
    double balance = acct.call<double>(get_balance_op{});
    std::cout << "Balance: " << balance << std::endl;  // 100.0

    // Withdraw
    bool success = acct.call<bool>(withdraw_op{}, 30.0);
    std::cout << "Withdraw: " << (success ? "OK" : "Failed") << std::endl;

    // Check new balance
    balance = acct.call<double>(get_balance_op{});
    std::cout << "New balance: " << balance << std::endl;  // 70.0

    cas::system::shutdown();
    cas::system::wait_for_shutdown();
}
```

## Return Types

### Primitive Types
```cpp
call_handler<int, op>(&actor::method);
call_handler<double, op>(&actor::method);
call_handler<bool, op>(&actor::method);
```

### Void Return
```cpp
call_handler<void, op>(&actor::method);

// Call site
actor.call<void>(op{}, args...);  // Returns void
```

### Custom Types
```cpp
struct result_data {
    int code;
    std::string message;
};

call_handler<result_data, op>(&actor::method);

// Call site
result_data res = actor.call<result_data>(op{}, args...);
```

### Containers
```cpp
call_handler<std::vector<int>, op>(&actor::method);
call_handler<std::map<std::string, double>, op>(&actor::method);
call_handler<std::optional<std::string>, op>(&actor::method);
```

## Error Handling

### Missing Handler

If no handler is registered for an operation, `call()` throws:

```cpp
try {
    int result = actor.call<int>(undefined_op{});
} catch (const std::runtime_error& e) {
    std::cerr << "Error: " << e.what() << std::endl;
    // "No call handler registered for operation"
}
```

### Invalid, Stopping or Stopped Target

- **Invalid `actor_ref`:** the plain form throws
  `std::runtime_error("Cannot call on invalid actor_ref")`. The timeout form
  returns `std::nullopt` and counts the request in
  `dead_letter_stats::dropped_to_invalid`.
- **Target stopping or stopped:** the request is dropped as a dead letter
  (counted in `dead_letter_stats::dropped_ask`) and the caller gets
  `std::future_error` (broken promise), with or without a timeout.

### Calling Yourself

An actor that calls itself always gets `cas::call_deadlock_error` (inline
actors included): call the method directly instead.

### Calling from on_start()

`call()` from inside `on_start()` throws `cas::on_start_violation` (a
`std::logic_error`): no worker is guaranteed to service the request yet. Use
`tell()` and handle the reply in a message handler. See
[Lifecycle Hooks](50_lifecycle_hooks.md).

### Type Mismatch

If the arguments or `ReturnType` do not match the registered handler exactly,
`call()` throws `cas::call_type_error` (a `std::logic_error`) instead of calling
the handler. With a timeout this is still an exception, not `std::nullopt`:

```cpp
// handler: double half(double x)
try {
    double h = actor.call<double>(half_op{}, 10);   // int, not double
} catch (const cas::call_type_error& e) {
    // "call() argument type mismatch for operation ... caller passed
    //  std::tuple<int>, handler takes std::tuple<double> ..."
}
double h = actor.call<double>(half_op{}, 10.0);     // OK
```

### Handler Exceptions

Exceptions from handlers propagate to the caller:

```cpp
class validator : public cas::actor {
protected:
    void on_start() override {
        call_handler<int, validate_op>(&validator::validate);
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
    int result = validator_ref.call<int>(validate_op{}, -5);
} catch (const std::invalid_argument& e) {
    std::cerr << "Validation failed: " << e.what() << std::endl;
}
```

## Actor-to-Actor Call

Actors can call other actors:

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
        // Call backend synchronously. msg.data must have exactly the type of
        // the process_op handler's parameter.
        std::string result;
        try {
            result = m_backend.call<std::string>(process_op{}, msg.data);
        } catch (const cas::call_deadlock_error&) {
            // backend shares this actor's worker thread - fall back to
            // tell() with a reply message (see below)
            return;
        }

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

## How Call Is Processed

A call request goes into the target actor's mailbox and is handled **next**:
right after the handler that is currently running, ahead of every ordinary
message still queued. Calls are handled in order among themselves. The request is
executed by the thread that runs the target - its pooled worker, a fast actor's
dedicated thread or, for an `inline_actor`, the calling thread itself; the
calling thread blocks on a future until the handler returns.

That is the point of `call()` beyond returning a value: it gets an answer even
when the actor is behind — including when the question is *why* it is behind.

What "next" does not cover:

- **A handler that is already running** is not interrupted; the call is handled
  when it returns.
- **Messages the caller sent earlier** may still be queued, and the call is
  handled before them:

  ```cpp
  position_keeper.tell(fill);
  int qty = position_keeper.call<int>(position_op{});  // may not include the fill yet
  ```

  If the answer must reflect an earlier message, send that message as a call
  too (calls are ordered among themselves), or design the handler to account
  for it.
- **Other actors on the same worker.** A pooled actor that is waiting for its
  turn runs after the actors queued ahead of it on that worker, each for at most
  `system_config::max_messages_per_turn` messages. For an actor that must always
  answer at once, use a `fast_actor` (its own thread).

This means a call handler is serialised with the actor's regular handlers: they
never run at the same time on the same actor, so both can touch member state
without locks. That is INVARIANT 1 — see `doc/INVARIANTS.md`.

> **Historical note:** an earlier version processed calls on a dedicated thread
> pool, dispatching directly into the target actor. That let a call handler and a
> regular handler run concurrently on one actor, which is a data race in any actor
> that mixes them. `system_config::ask_thread_pool_size` still exists so old
> configuration code compiles, but it is ignored.

### Calling call() from inside a handler

Safe **only when the thread you are on is not the one that runs the target**.
The target's own thread must run the handler, so blocking that thread means the
request is never serviced. The check is about threads, not actors: a
`fast_actor` may call any pooled actor, and an inline actor's handler runs on
the thread that sent to it - so from there, calling a pooled actor on that
same worker is refused.

Such a call is detected and throws `cas::call_deadlock_error` immediately
rather than hanging:

```cpp
void on_request(const request& msg) {
    try {
        int result = other_actor.call<int>(compute_op{}, 42);
    } catch (const cas::call_deadlock_error& e) {
        // Target shares this actor's thread - use tell() with a reply instead
    }
}
```

Calling an `inline_actor` never waits for a worker - its handler runs on your
thread before `call()` returns - so it is always allowed, except into an inline
actor that is already handling a message on this thread (a cycle back into
it), which throws `call_deadlock_error`.

Actor assignment to threads is round-robin and not something you control, so a
handler that calls another pooled actor **may throw depending on placement**. For
actor-to-actor communication, prefer `tell()` with an explicit reply message —
it is non-blocking and never has this problem. Reserve `call()` for calls from
outside the actor system (main thread, test code, request handlers).

## Performance Considerations

### When to Use Call

**Use call for:**
- ✓ Operations needing immediate results
- ✓ Synchronous APIs (REST endpoints, CLI commands)
- ✓ Database queries
- ✓ Calculations with dependencies
- ✓ Validation that must complete before continuing

**Don't use call for:**
- ✗ Fire-and-forget operations (use `tell()` instead)
- ✗ High-frequency operations (async messages are faster)
- ✗ Long-running operations without timeout
- ✗ When you can restructure to use callbacks

### Performance Comparison

```cpp
// Call (synchronous) - one round trip (sub-µs to a few µs, see the README's
// Benchmarks) plus a promise/future, and the caller is blocked meanwhile
int result = actor.call<int>(op{}, arg);  // Blocks

// Regular message (asynchronous) - enqueue only
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
        // Synchronous database query. The handler returns std::optional<user>
        // (empty = not found); the timeout form wraps that in another optional
        // (empty = timed out).
        auto user_data = m_database.call<std::optional<user>>(
            query_user_op{},
            std::chrono::seconds(5),  // Timeout
            user_id
        );

        if (!user_data) {
            return "{\"error\": \"timeout\"}";
        }
        if (!*user_data) {
            return "{\"error\": \"User not found\"}";
        }
        return serialize_to_json(**user_data);
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

        call_handler<std::string, get_config_op>(&config_manager::get_value);
        call_handler<void, set_config_op>(&config_manager::set_value);
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
// Arguments must have exactly the parameter types: std::string, not a literal
std::string db_host = config.call<std::string>(get_config_op{}, std::string("database.host"));
```

## Best Practices

### DO: Use Timeouts for External Operations

```cpp
auto result = actor.call<data>(
    fetch_op{},
    std::chrono::seconds(10),  // Generous timeout
    url
);

if (!result.has_value()) {
    handle_timeout();
}
```

### DO: Keep Call Handlers Fast

```cpp
// ✓ GOOD: Fast computation
int calculate(int a, int b) {
    return a + b;
}

// ✗ BAD: Slow blocking operation
std::string fetch_from_network(const std::string& url) {
    // This occupies the target actor's worker thread for the whole call,
    // stalling every other actor assigned to that thread.
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

### DON'T: Overuse Call

```cpp
// ✗ BAD: Using call when async would work
for (int i = 0; i < 1000; ++i) {
    actor.call<void>(process_op{}, i);  // Synchronous, slow!
}

// ✓ GOOD: Use async messages
for (int i = 0; i < 1000; ++i) {
    process_msg m;
    m.value = i;
    actor.tell(m);  // Asynchronous, fast!
}
```

## Migrating from 1.0 (`ask`)

`call()` was called `ask()` in 1.0. The old names still compile and forward to
the new ones, with a deprecation warning that names the replacement:

| 1.0 name | Current name |
|----------|--------------|
| `actor_ref::ask()` | `actor_ref::call()` |
| `ask_handler()` | `call_handler()` |
| `cas::ask_deadlock_error` | `cas::call_deadlock_error` |
| `cas::ask_type_error` | `cas::call_type_error` |
| `ask_queue_high_water_mark()` | `call_queue_high_water_mark()` |
| `cas/ask_message.h` | `cas/call_message.h` |

The header `cas/ask_message.h` only includes `cas/call_message.h` and gives no
warning. `dead_letter_stats::dropped_ask` keeps its name (a struct field cannot
be aliased).

## Comparison: Call vs Regular Messages

| Feature | Call | Regular Messages |
|---------|------------|------------------|
| Execution | Synchronous (blocks) | Asynchronous (returns immediately) |
| Return value | Yes | No (use callbacks) |
| Overhead | One round trip + blocking | Enqueue only |
| Runs on | Target actor's worker thread | Target actor's worker thread |
| Queue | Actor's mailbox, handled next (ahead of queued messages) | Actor's mailbox, in send order |
| Timeout support | Yes | No |
| Use case | Need immediate result | Fire-and-forget |

## Next Steps

- [Timers](90_timers.md) - Schedule delayed and periodic messages
- [Advanced Actors](100_advanced_actors.md) - Fast, inline, and stateful actors
- [Best Practices](120_best_practices.md) - Design patterns and guidelines
