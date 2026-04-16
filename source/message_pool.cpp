#include "cas/message_pool.h"
#include <algorithm>

namespace cas {

// Static member definitions
std::array<message_pool::size_class_pool, message_pool::NUM_SIZE_CLASSES> message_pool::s_pools;
std::atomic<size_t> message_pool::s_max_blocks_per_class{message_pool::DEFAULT_MAX_BLOCKS_PER_CLASS};
std::atomic<size_t> message_pool::s_pool_hits{0};
std::atomic<size_t> message_pool::s_pool_misses{0};
std::atomic<size_t> message_pool::s_heap_fallbacks{0};
std::atomic<size_t> message_pool::s_pool_full_frees{0};
std::atomic<bool> message_pool::s_initialized{false};

void message_pool::init_pools() {
    // Double-checked locking pattern
    if (s_initialized.load(std::memory_order_acquire)) {
        return;
    }

    // Initialize pool block sizes
    for (size_t i = 0; i < NUM_SIZE_CLASSES; ++i) {
        s_pools[i].block_size = SIZE_CLASSES[i];
        s_pools[i].head.store(nullptr, std::memory_order_relaxed);
    }

    s_initialized.store(true, std::memory_order_release);
}

size_t message_pool::size_class_index(size_t size) {
    // Find smallest size class that fits
    for (size_t i = 0; i < NUM_SIZE_CLASSES; ++i) {
        if (size <= SIZE_CLASSES[i]) {
            return i;
        }
    }
    // Should not reach here for pooled sizes
    return NUM_SIZE_CLASSES;
}

void* message_pool::allocate(size_t size) {
    // Ensure pools are initialized
    if (!s_initialized.load(std::memory_order_acquire)) {
        init_pools();
    }

    // Oversized - fall back to heap
    if (size > MAX_POOLED_SIZE) {
        s_heap_fallbacks.fetch_add(1, std::memory_order_relaxed);
        return ::operator new(size);
    }

    size_t idx = size_class_index(size);
    size_class_pool& pool = s_pools[idx];

    // Try to pop from free list (lock-free)
    free_node* old_head = pool.head.load(std::memory_order_acquire);
    while (old_head != nullptr) {
        // Attempt to CAS head to next node
        if (pool.head.compare_exchange_weak(
                old_head,
                old_head->next,
                std::memory_order_acquire,
                std::memory_order_acquire)) {
            // Successfully popped from pool
            pool.count.fetch_sub(1, std::memory_order_relaxed);
            s_pool_hits.fetch_add(1, std::memory_order_relaxed);
            return old_head;
        }
        // CAS failed, old_head updated by compare_exchange_weak, retry
    }

    // Pool empty - allocate new block from heap
    s_pool_misses.fetch_add(1, std::memory_order_relaxed);
    return ::operator new(pool.block_size);
}

void message_pool::deallocate(void* ptr, size_t size) {
    if (ptr == nullptr) {
        return;
    }

    // Oversized - return to heap
    if (size > MAX_POOLED_SIZE) {
        ::operator delete(ptr);
        return;
    }

    size_t idx = size_class_index(size);
    size_class_pool& pool = s_pools[idx];

    // Check if pool is at capacity
    size_t max_blocks = s_max_blocks_per_class.load(std::memory_order_relaxed);
    if (max_blocks > 0 && pool.count.load(std::memory_order_relaxed) >= max_blocks) {
        // Pool full - free to OS instead
        s_pool_full_frees.fetch_add(1, std::memory_order_relaxed);
        ::operator delete(ptr);
        return;
    }

    // Push to free list (lock-free)
    free_node* node = static_cast<free_node*>(ptr);
    node->next = pool.head.load(std::memory_order_relaxed);

    while (!pool.head.compare_exchange_weak(
            node->next,
            node,
            std::memory_order_release,
            std::memory_order_relaxed)) {
        // CAS failed, node->next updated, retry
    }

    pool.count.fetch_add(1, std::memory_order_relaxed);
}

void message_pool::prewarm(size_t count_per_class) {
    // Ensure pools are initialized
    if (!s_initialized.load(std::memory_order_acquire)) {
        init_pools();
    }

    // Pre-allocate blocks for each size class
    for (size_t i = 0; i < NUM_SIZE_CLASSES; ++i) {
        size_t block_size = SIZE_CLASSES[i];

        for (size_t j = 0; j < count_per_class; ++j) {
            void* block = ::operator new(block_size);
            deallocate(block, block_size);
        }
    }
}

void message_pool::set_max_pool_size(size_t max_blocks_per_class) {
    s_max_blocks_per_class.store(max_blocks_per_class, std::memory_order_relaxed);
}

size_t message_pool::get_max_pool_size() {
    return s_max_blocks_per_class.load(std::memory_order_relaxed);
}

message_pool::stats message_pool::get_stats() {
    stats s;
    s.pool_hits = s_pool_hits.load(std::memory_order_relaxed);
    s.pool_misses = s_pool_misses.load(std::memory_order_relaxed);
    s.heap_fallbacks = s_heap_fallbacks.load(std::memory_order_relaxed);
    s.pool_full_frees = s_pool_full_frees.load(std::memory_order_relaxed);
    return s;
}

void message_pool::reset_stats() {
    s_pool_hits.store(0, std::memory_order_relaxed);
    s_pool_misses.store(0, std::memory_order_relaxed);
    s_heap_fallbacks.store(0, std::memory_order_relaxed);
    s_pool_full_frees.store(0, std::memory_order_relaxed);
}

} // namespace cas
