#include "cas/message_pool.h"
#include "external/concurrentqueue.h"
#include <array>
#include <optional>

namespace cas {

namespace {

// Design: a small per-thread cache in front of one shared free list per size
// class.
//
// The shared free list is a moodycamel::ConcurrentQueue of block pointers. It
// replaced a hand-rolled Treiber stack whose pop - load head, read head->next,
// CAS head to next - was open to ABA: between the read and the CAS, other
// threads could pop that block and the next one and push the first back, so
// the CAS succeeded and installed a block that was already in use as the new
// head. The block was then handed out a second time, and user data written
// into it overwrote the free list's next pointer. With two threads that both
// allocated and freed, the pool was corrupted within milliseconds.
// ConcurrentQueue is a proven lock-free MPMC structure, already vendored for
// the mailboxes, so the project does not have to get its own right.
//
// The per-thread cache keeps the common case off the shared queue altogether:
// each thread allocates from and frees into a private array (no atomics), and
// only moves blocks to or from the queue in batches. Messages usually flow
// one way - allocated by the sender's thread, freed by the receiver's - so
// the receiver's cache fills and flushes a batch, and the sender's empties
// and refills one.

constexpr size_t k_cache_capacity = 32;  // blocks per thread per size class
constexpr size_t k_transfer_batch = 16;  // blocks moved per queue operation

struct size_class_pool {
    moodycamel::ConcurrentQueue<void*> free_blocks;

    // Blocks currently held in free_blocks, for the max-pool-size cap.
    // Incremented before blocks are enqueued and decremented after they are
    // dequeued, so it never under-counts and cannot wrap below zero. Blocks
    // in per-thread caches are not counted (at most k_cache_capacity per
    // thread per class).
    std::atomic<size_t> count{0};

    size_t block_size = 0;
};

struct pool_state {
    std::array<size_class_pool, message_pool::NUM_SIZE_CLASSES> classes;

    std::atomic<size_t> max_blocks_per_class{message_pool::DEFAULT_MAX_BLOCKS_PER_CLASS};

    std::atomic<size_t> pool_hits{0};
    std::atomic<size_t> pool_misses{0};
    std::atomic<size_t> heap_fallbacks{0};
    std::atomic<size_t> pool_full_frees{0};

    pool_state() {
        for (size_t i = 0; i < message_pool::NUM_SIZE_CLASSES; ++i) {
            classes[i].block_size = message_pool::SIZE_CLASSES[i];
        }
    }
};

// Deliberately never destroyed. Messages can be freed during static
// destruction - the actor system singleton is torn down at exit with messages
// still queued - which may be after a function-local static pool had already
// been destroyed. The pooled blocks themselves were never returned to the OS
// before exit either.
pool_state& state() {
    static pool_state* const s = new pool_state();
    return *s;
}

// Index of the smallest size class that fits, or NUM_SIZE_CLASSES if none.
size_t size_class_index(size_t size) {
    for (size_t i = 0; i < message_pool::NUM_SIZE_CLASSES; ++i) {
        if (size <= message_pool::SIZE_CLASSES[i]) {
            return i;
        }
    }
    return message_pool::NUM_SIZE_CLASSES;
}

// Return blocks to the shared free list, or to the heap beyond the cap.
void release_to_shared(size_class_pool& pool, void** blocks, size_t n,
                       moodycamel::ProducerToken* producer) {
    auto& st = state();
    const size_t max_blocks = st.max_blocks_per_class.load(std::memory_order_relaxed);

    // Reserve room under the cap for as many as fit; free the rest.
    size_t keep = n;
    const size_t previous = pool.count.fetch_add(n, std::memory_order_relaxed);
    if (max_blocks > 0 && previous + n > max_blocks) {
        keep = previous >= max_blocks ? 0 : max_blocks - previous;
        pool.count.fetch_sub(n - keep, std::memory_order_relaxed);
        for (size_t i = keep; i < n; ++i) {
            ::operator delete(blocks[i]);
        }
        st.pool_full_frees.fetch_add(n - keep, std::memory_order_relaxed);
    }
    if (keep == 0) {
        return;
    }

    // enqueue_bulk() allocates queue storage when it needs to grow; if that
    // fails, give the blocks back to the heap rather than lose them.
    const bool queued = producer
        ? pool.free_blocks.enqueue_bulk(*producer, blocks, keep)
        : pool.free_blocks.enqueue_bulk(blocks, keep);
    if (!queued) {
        pool.count.fetch_sub(keep, std::memory_order_relaxed);
        for (size_t i = 0; i < keep; ++i) {
            ::operator delete(blocks[i]);
        }
    }
}

// This thread's cache and queue tokens.
struct thread_cache {
    struct class_cache {
        std::array<void*, k_cache_capacity> blocks{};
        size_t size = 0;
        std::optional<moodycamel::ProducerToken> producer;
        std::optional<moodycamel::ConsumerToken> consumer;
    };
    std::array<class_cache, message_pool::NUM_SIZE_CLASSES> classes;

    ~thread_cache() {
        // Hand cached blocks back so other threads can use them.
        auto& st = state();
        for (size_t i = 0; i < classes.size(); ++i) {
            class_cache& c = classes[i];
            if (c.size > 0) {
                release_to_shared(st.classes[i], c.blocks.data(), c.size,
                                  c.producer ? &*c.producer : nullptr);
                c.size = 0;
            }
        }
    }
};

// Lifetime: messages are freed during static destruction, after this thread's
// thread_local objects with destructors may already be gone, and touching one
// then is undefined. So the cache is reached through a trivially destructible
// pointer, which stays safe to read for the life of the thread; a separate
// thread_local deletes it at thread exit and marks it gone, after which the
// shared free list is used directly.
thread_local thread_cache* t_cache = nullptr;
thread_local bool t_cache_released = false;

struct thread_cache_release {
    ~thread_cache_release() {
        thread_cache* cache = t_cache;
        t_cache = nullptr;
        t_cache_released = true;  // before delete: its flush must not recreate it
        delete cache;
    }
};
thread_local thread_cache_release t_cache_release;

thread_cache* current_cache() {
    if (t_cache == nullptr && !t_cache_released) {
        t_cache = new thread_cache();
        (void)&t_cache_release;  // odr-use: registers its destructor for this thread
    }
    return t_cache;
}

} // namespace

void* message_pool::allocate(size_t size) {
    auto& st = state();

    // Oversized - fall back to heap
    if (size > MAX_POOLED_SIZE) {
        st.heap_fallbacks.fetch_add(1, std::memory_order_relaxed);
        return ::operator new(size);
    }

    const size_t idx = size_class_index(size);
    size_class_pool& pool = st.classes[idx];

    if (thread_cache* cache = current_cache()) {
        auto& c = cache->classes[idx];
        if (c.size == 0) {
            // Refill a batch from the shared free list
            if (!c.consumer) c.consumer.emplace(pool.free_blocks);
            c.size = pool.free_blocks.try_dequeue_bulk(*c.consumer, c.blocks.data(),
                                                       k_transfer_batch);
            pool.count.fetch_sub(c.size, std::memory_order_relaxed);
        }
        if (c.size > 0) {
            st.pool_hits.fetch_add(1, std::memory_order_relaxed);
            return c.blocks[--c.size];
        }
    } else {
        // Thread is exiting: no cache, use the shared list directly
        void* block = nullptr;
        if (pool.free_blocks.try_dequeue(block)) {
            pool.count.fetch_sub(1, std::memory_order_relaxed);
            st.pool_hits.fetch_add(1, std::memory_order_relaxed);
            return block;
        }
    }

    // Pool empty - allocate new block from heap
    st.pool_misses.fetch_add(1, std::memory_order_relaxed);
    return ::operator new(pool.block_size);
}

void message_pool::deallocate(void* ptr, size_t size) {
    if (ptr == nullptr) {
        return;
    }

    auto& st = state();

    // Oversized - return to heap
    if (size > MAX_POOLED_SIZE) {
        ::operator delete(ptr);
        return;
    }

    const size_t idx = size_class_index(size);
    size_class_pool& pool = st.classes[idx];

    if (thread_cache* cache = current_cache()) {
        auto& c = cache->classes[idx];
        if (c.size == k_cache_capacity) {
            // Full: flush the most recently freed batch to the shared free
            // list. In one-way traffic this thread only frees, so it would
            // never reuse them; the allocating thread gets the warmest blocks.
            if (!c.producer) c.producer.emplace(pool.free_blocks);
            c.size -= k_transfer_batch;
            release_to_shared(pool, c.blocks.data() + c.size, k_transfer_batch, &*c.producer);
        }
        c.blocks[c.size++] = ptr;
        return;
    }

    // Thread is exiting: no cache, use the shared list directly
    release_to_shared(pool, &ptr, 1, nullptr);
}

void message_pool::prewarm(size_t count_per_class) {
    // Blocks go straight to the shared free lists (not this thread's cache),
    // so every thread benefits.
    std::array<void*, k_transfer_batch> batch;
    for (size_t i = 0; i < NUM_SIZE_CLASSES; ++i) {
        size_t remaining = count_per_class;
        while (remaining > 0) {
            const size_t n = remaining < batch.size() ? remaining : batch.size();
            for (size_t j = 0; j < n; ++j) {
                batch[j] = ::operator new(SIZE_CLASSES[i]);
            }
            release_to_shared(state().classes[i], batch.data(), n, nullptr);
            remaining -= n;
        }
    }
}

void message_pool::set_max_pool_size(size_t max_blocks_per_class) {
    state().max_blocks_per_class.store(max_blocks_per_class, std::memory_order_relaxed);
}

size_t message_pool::get_max_pool_size() {
    return state().max_blocks_per_class.load(std::memory_order_relaxed);
}

message_pool::stats message_pool::get_stats() {
    auto& st = state();
    stats s;
    s.pool_hits = st.pool_hits.load(std::memory_order_relaxed);
    s.pool_misses = st.pool_misses.load(std::memory_order_relaxed);
    s.heap_fallbacks = st.heap_fallbacks.load(std::memory_order_relaxed);
    s.pool_full_frees = st.pool_full_frees.load(std::memory_order_relaxed);
    return s;
}

void message_pool::reset_stats() {
    auto& st = state();
    st.pool_hits.store(0, std::memory_order_relaxed);
    st.pool_misses.store(0, std::memory_order_relaxed);
    st.heap_fallbacks.store(0, std::memory_order_relaxed);
    st.pool_full_frees.store(0, std::memory_order_relaxed);
}

} // namespace cas
