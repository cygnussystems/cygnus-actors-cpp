# Message Pool

Messages allocate through a lock-free size-class pool instead of the general heap.
`message_base` overrides `operator new`/`operator delete`, so this applies to every
message automatically — including those created by `std::make_unique` and by
`tell()`, which copies your message onto the heap internally.

This document describes what the pool actually does, because it is easy to assume
more than it provides.

---

## What the pool is — and is not

The pool is a **size-class free list**. It recycles raw memory blocks.

It is **not** object reuse. Every message is still constructed and destructed
normally; only the underlying block is kept on a free list rather than returned to
the OS. Constructors and destructors run exactly as they would without the pool.

What you get: allocation and deallocation become a lock-free pop/push on an atomic
free list — O(1), no `malloc` lock, no page faults after warm-up.

What you do not get: skipped construction, skipped destruction, or any reuse of the
object's contents.

---

## Header overhead — the part that surprises people

`message_base::operator new` prepends a size header to every allocation so
`operator delete` can return the block to the right size class.

**The size class is chosen from `HEADER_OVERHEAD + sizeof(YourMessage)`, not from
`sizeof(YourMessage)` alone.**

The header is `sizeof(size_t)` rounded up to `alignof(std::max_align_t)`, which is
**platform-dependent**:

| Platform | `alignof(std::max_align_t)` | `HEADER_OVERHEAD` |
|---|---|---|
| MSVC x64 | 8 (`long double` == `double`) | **8 bytes** |
| Linux / GCC / Clang x86-64 | 16 (`long double` is 16-aligned) | **16 bytes** |

Do not hard-code either value. Use `cas::message_pool::HEADER_OVERHEAD`, which is
`constexpr` and correct for the platform you are compiling for.

### The consequence

A message sized to exactly a size-class boundary **lands in the next class up**:

```cpp
struct my_msg : public cas::message_base {
    // ... members totalling exactly 128 bytes
};

// On Linux (16-byte header): 16 + 128 = 144  -> 256-byte class
// On MSVC   ( 8-byte header):  8 + 128 = 136 -> 256-byte class
```

Either way you consume a 256-byte block to carry a 128-byte message — roughly
double the memory you budgeted for. If you are sizing hot messages to fit a class,
you must account for the header.

### Sizing messages correctly

Two `constexpr` helpers make this a compile-time check:

```cpp
// Largest payload that still fits a given class
constexpr size_t cas::message_pool::max_payload_for_class(size_t size_class);

// Which class a payload will actually land in (0 = exceeds pool, goes to heap)
constexpr size_t cas::message_pool::size_class_for_payload(size_t payload_size);
```

Pin the constraint with a `static_assert` so a future member addition fails the
build instead of silently doubling your memory use:

```cpp
struct tick_msg : public cas::message_base {
    uint64_t timestamp;
    double   price;
    uint32_t volume;
};

static_assert(sizeof(tick_msg) <= cas::message_pool::max_payload_for_class(128),
              "tick_msg no longer fits the 128-byte size class");
```

Note that `sizeof(cas::message_base)` is itself 40 bytes (an `actor_ref` plus two
`uint64_t` plus a vtable pointer), so your own members start from that baseline.

---

## Members that allocate outside the pool

The pool covers the message object itself. It does **not** cover memory that the
message's members allocate on their own.

```cpp
struct bad_msg : public cas::message_base {
    std::string symbol;        // heap-allocates unless SSO applies
    std::vector<double> bars;  // always heap-allocates when non-empty
};
```

Sending one of these performs a pooled allocation for the message *plus* one or
more ordinary heap allocations for the members — defeating the purpose on a hot
path.

**Use `cas::fixed_string` instead of `std::string`** for message fields. It stores
its characters inline, so the whole message stays within its pooled block:

```cpp
#include "cas/fixed_string.h"

struct good_msg : public cas::message_base {
    cas::fixed_string<16> symbol;  // inline, no heap allocation
    double price;
};
```

For variable-length collections, prefer a fixed-capacity inline array. If the
count is genuinely unbounded, see "Oversized messages" below.

---

## Size classes and the heap fallback

```
64, 128, 256, 512, 1024 bytes    (MAX_POOLED_SIZE = 1024)
```

An allocation of `HEADER_OVERHEAD + sizeof(msg)` above **1024 bytes** bypasses the
pool entirely and goes to `::operator new`. This is counted in
`message_pool::get_stats().heap_fallbacks`.

Nothing breaks when this happens — the message is delivered normally — but you lose
the pool's benefit, so it is worth knowing about rather than discovering under load.

### Oversized messages

If a message must carry a variable number of elements that can exceed the 1024-byte
budget, the options are:

1. **Fixed-capacity inline array with a spill path** — carry up to N inline, and for
   larger payloads send a handle to separately-owned storage.
2. **Refcounted payload handle** — the message carries a `shared_ptr` to the bulk
   data. The message itself stays small and pooled; the payload is allocated once
   and shared rather than copied per send.
3. **Split into multiple messages** — but note there is **no ordering guarantee
   between messages** (see [40_message_passing.md](40_message_passing.md)), so the
   receiver must handle out-of-order arrival explicitly.

Option 2 is usually the right answer for bulk market data.

---

## Monitoring

```cpp
auto stats = cas::message_pool::get_stats();
stats.pool_hits;       // served from a free list - the fast path
stats.pool_misses;     // pool was empty, allocated a new block
stats.heap_fallbacks;  // exceeded MAX_POOLED_SIZE, bypassed the pool
stats.pool_full_frees; // pool at capacity, block returned to the OS
```

A healthy steady state is `pool_hits` dominating, with `pool_misses` flattening
after warm-up. Persistent `heap_fallbacks` means messages are exceeding 1024 bytes.
Persistent `pool_full_frees` means the per-class cap is too low for your working set.

### Pre-warming

`pool_misses` is highest at startup, when every class is empty. Pre-allocate to move
that cost out of the hot path:

```cpp
cas::message_pool::prewarm(256);  // 256 blocks per size class
```

### Capacity

```cpp
cas::message_pool::set_max_pool_size(10000);  // per size class; 0 = unlimited
```

Default is 10,000 blocks per class (roughly 25 MB across all five classes if every
class fills). Beyond the cap, `deallocate` frees to the OS instead of pooling.

---

## Summary

| Property | Reality |
|---|---|
| Mechanism | Size-class free list, lock-free |
| Object reuse | No — construction and destruction always run |
| Header overhead | `HEADER_OVERHEAD`: 8 (MSVC x64) / 16 (Linux x86-64) |
| Size class chosen from | `HEADER_OVERHEAD + sizeof(msg)` — not `sizeof(msg)` |
| Size classes | 64, 128, 256, 512, 1024 |
| Above 1024 bytes | Heap fallback, counted in `heap_fallbacks` |
| `std::string` / `std::vector` members | Allocate outside the pool — use `fixed_string` |
