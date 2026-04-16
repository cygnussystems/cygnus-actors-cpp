#ifndef CAS_MESSAGE_POOL_H
#define CAS_MESSAGE_POOL_H

#include <atomic>
#include <array>
#include <cstddef>
#include <new>

namespace cas {

/// Lock-free memory pool for message allocation
/// Uses size-class buckets with atomic free-lists for O(1) alloc/dealloc
/// Eliminates heap allocation overhead for high-frequency messaging
class message_pool {
public:
    /// Size classes for pooled allocation (bytes)
    static constexpr size_t SIZE_CLASSES[] = {64, 128, 256, 512, 1024};
    static constexpr size_t MAX_POOLED_SIZE = 1024;
    static constexpr size_t NUM_SIZE_CLASSES = 5;

    /// Allocate memory from pool
    /// Falls back to heap for sizes > MAX_POOLED_SIZE
    static void* allocate(size_t size);

    /// Return memory to pool
    /// size must match the original allocation size
    static void deallocate(void* ptr, size_t size);

    /// Pre-warm pools by allocating blocks upfront
    /// Call at startup to avoid first-allocation latency
    static void prewarm(size_t count_per_class = 256);

    /// Get pool statistics (for debugging/monitoring)
    struct stats {
        size_t pool_hits = 0;      // Allocations served from pool
        size_t pool_misses = 0;    // Allocations that needed new block
        size_t heap_fallbacks = 0; // Oversized allocations (heap)
    };
    static stats get_stats();
    static void reset_stats();

private:
    /// Free list node - stored in the free block itself
    struct free_node {
        free_node* next;
    };

    /// Per-size-class pool with atomic head pointer
    struct alignas(64) size_class_pool {  // Cache-line aligned to avoid false sharing
        std::atomic<free_node*> head{nullptr};
        size_t block_size = 0;
    };

    /// Global pools for each size class
    static std::array<size_class_pool, NUM_SIZE_CLASSES> s_pools;

    /// Statistics (atomic for thread safety)
    static std::atomic<size_t> s_pool_hits;
    static std::atomic<size_t> s_pool_misses;
    static std::atomic<size_t> s_heap_fallbacks;

    /// Get pool index for a given size
    static size_t size_class_index(size_t size);

    /// Initialize pools (called automatically)
    static void init_pools();
    static std::atomic<bool> s_initialized;
};

} // namespace cas

#endif // CAS_MESSAGE_POOL_H
