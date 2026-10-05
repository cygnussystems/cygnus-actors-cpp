# Message Pool

Messages allocate through a lock-free size-class pool instead of the general heap.
`message_base` overrides `operator new`/`operator delete`, so this applies to every
message automatically — including those created by `std::make_unique` and by
`tell()`, which copies your message onto the heap internally.

This document describes what the pool actually does, because it is easy to assume
more than it provides.

> **Runnable version:** `tutorials/12_message_pool.cpp` — run `tutorials 12`.
> It prints real header overhead, size-class assignment and pool statistics
> for the machine you are on, rather than quoting figures.

---

## What the pool is — and is not

The pool is a **size-class free list**. It recycles raw memory blocks.

It is **not** object reuse. Every message is still constructed and destructed
normally; only the underlying block is kept on a free list rather than returned to
the OS. Constructors and destructors run exactly as they would without the pool.

What you get: allocation and deallocation are O(1), take no `malloc` lock, and
cause no page faults after warm-up.

### How it is built

- **Per-thread cache.** Each thread keeps up to 32 blocks per size class in a
  private array. Allocating pops from it and freeing pushes to it, with no atomic
  operations — the common case.
- **Shared free list.** One per size class, a lock-free MPMC queue
  (`moodycamel::ConcurrentQueue`). Caches refill from it and flush to it
  **16 blocks at a time**, so it is touched once per 16 allocations. Messages usually flow one way — allocated on the sender's
  thread, freed on the receiver's — and batching is what makes that cheap.
- A thread's cached blocks go back to the shared list when the thread exits.

A consequence: right after start, blocks freed by a receiver sit in its cache
until the cache is full (32), and only then do 16 move to the shared list, so the first workload in a size class takes a few more pool
misses than it otherwise would. Call `prewarm()` if the first messages after start
are latency-sensitive — prewarmed blocks go straight to the shared lists.

> Before this design the free list was a hand-rolled lock-free stack. Its pop was
> open to ABA, and with two threads that both allocated and freed, it handed one
> block to two owners and corrupted itself within milliseconds.
> `tests/13_message_pool/` now covers that case.

What you do not get: skipped construction, skipped destruction, or any reuse of the
object's contents.

---

## Header overhead — the part that surprises people

`message_base::operator new` prepends a size header to every allocation so
`operator delete` can return the block to the right size class.

**The size class is chosen from `HEADER_OVERHEAD + sizeof(YourMessage)`, not from
`sizeof(YourMessage)` alone.**

The header is `sizeof(size_t)` rounded up to
`cas::message_pool::POOLED_ALIGNMENT` — the alignment plain `operator new`
guarantees (`__STDCPP_DEFAULT_NEW_ALIGNMENT__`) — so the message after it keeps
that alignment. On every x64 toolchain that is **16 bytes**.

Do not hard-code the value. Use `cas::message_pool::HEADER_OVERHEAD`, which is
`constexpr` and correct for the platform you are compiling for.

> Earlier versions padded the header to `alignof(std::max_align_t)` instead. That
> is 8 on MSVC, so every pooled message there sat 8 bytes off a 16-byte boundary,
> and a message with `alignas(16)` members was misaligned. On MSVC the header is
> now 16 bytes rather than 8, so a message within 8 bytes of a class boundary
> moves up one class.

### Over-aligned messages

A message whose alignment exceeds `POOLED_ALIGNMENT` — for example `alignas(64)` to
give it its own cache line — **bypasses the pool** and is allocated with aligned
`operator new`. It is correctly aligned, but gets none of the pool's benefits and
is not counted in the pool statistics.

### The consequence

A message sized to exactly a size-class boundary **lands in the next class up**:

```cpp
struct my_msg : public cas::message_base {
    // ... members totalling exactly 128 bytes
};

// With the 16-byte header: 16 + 128 = 144 -> 256-byte class
```

You consume a 256-byte block to carry a 128-byte message — roughly
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

Note that `sizeof(cas::message_base)` is itself 48 bytes on x64 (an `actor_ref`,
two `uint64_t`, a vtable pointer and the mailbox link), so your own members start
from that baseline.

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
3. **Split into multiple messages** — parts sent by one sender arrive in order
   (see [40_message_passing.md](40_message_passing.md)), but parts from different
   senders can interleave.

Option 2 is usually the right answer for bulk market data. Make the payload
`const` (`std::shared_ptr<const T>`), because every receiver reads the same
object. [40_message_passing.md](40_message_passing.md#large-payloads-share-them-dont-copy-them)
covers the rules, including which thread frees the payload. The runnable
version is `tutorials/13_shared_payload.cpp`.

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
The cap covers the shared free lists only: blocks held in per-thread caches (up
to 32 per size class per thread) are not counted against it.

---

## Summary

| Property | Reality |
|---|---|
| Mechanism | Per-thread caches over a lock-free shared free list per size class |
| Object reuse | No — construction and destruction always run |
| Header overhead | `HEADER_OVERHEAD`: 16 on x64 (MSVC, GCC, Clang) |
| Alignment | `POOLED_ALIGNMENT` (16 on x64); over-aligned messages bypass the pool |
| Size class chosen from | `HEADER_OVERHEAD + sizeof(msg)` — not `sizeof(msg)` |
| Size classes | 64, 128, 256, 512, 1024 |
| Above 1024 bytes | Heap fallback, counted in `heap_fallbacks` |
| `std::string` / `std::vector` members | Allocate outside the pool — use `fixed_string` |
