#ifndef CAS_MESSAGE_POOL_H
#define CAS_MESSAGE_POOL_H

#include <atomic>
#include <array>
#include <cstddef>
#include <new>

namespace cas {

/// Lock-free memory pool for message allocation
/// Uses size-class buckets with lock-free free lists for O(1) alloc/dealloc
/// Eliminates heap allocation overhead for high-frequency messaging
class message_pool {
public:
    /// Size classes for pooled allocation (bytes)
    static constexpr size_t SIZE_CLASSES[] = {64, 128, 256, 512, 1024};
    static constexpr size_t MAX_POOLED_SIZE = 1024;
    static constexpr size_t NUM_SIZE_CLASSES = 5;

    /// Default max blocks per size class (0 = unlimited)
    /// 10000 blocks × 5 classes × avg 512 bytes ≈ 25MB max pool memory
    static constexpr size_t DEFAULT_MAX_BLOCKS_PER_CLASS = 10000;

    /// Alignment of every pooled message: the alignment plain operator new
    /// guarantees (__STDCPP_DEFAULT_NEW_ALIGNMENT__, 16 on x64). Types that
    /// need more are over-aligned and bypass the pool (see message_base).
    ///
    /// Deliberately not alignof(std::max_align_t): that is 8 on MSVC, which
    /// placed every pooled message 8 bytes off a 16-byte boundary even though
    /// the language treats alignas(16) types as not over-aligned.
    static constexpr size_t POOLED_ALIGNMENT = __STDCPP_DEFAULT_NEW_ALIGNMENT__;

    /// Per-allocation header overhead added by message_base::operator new.
    /// sizeof(size_t) rounded up to POOLED_ALIGNMENT - 16 bytes on x64.
    /// The size class is chosen from (HEADER_OVERHEAD + sizeof(YourMessage)),
    /// NOT from sizeof(YourMessage) alone.
    static constexpr size_t HEADER_OVERHEAD =
        (sizeof(size_t) + POOLED_ALIGNMENT - 1) & ~(POOLED_ALIGNMENT - 1);

    /// Largest message that still fits in the given size class.
    /// Use to size hot messages at compile time:
    ///
    ///     static_assert(sizeof(my_msg) <= cas::message_pool::max_payload_for_class(128),
    ///                   "my_msg no longer fits the 128-byte class");
    ///
    /// Returns 0 if the header alone exceeds the class.
    static constexpr size_t max_payload_for_class(size_t size_class) {
        return size_class > HEADER_OVERHEAD ? size_class - HEADER_OVERHEAD : 0;
    }

    /// Size class a message of the given payload size will actually land in.
    /// Returns 0 when the allocation exceeds MAX_POOLED_SIZE and goes to the heap.
    static constexpr size_t size_class_for_payload(size_t payload_size) {
        return (payload_size + HEADER_OVERHEAD) > MAX_POOLED_SIZE
            ? 0
            : ((payload_size + HEADER_OVERHEAD) <= 64   ? 64
             : (payload_size + HEADER_OVERHEAD) <= 128  ? 128
             : (payload_size + HEADER_OVERHEAD) <= 256  ? 256
             : (payload_size + HEADER_OVERHEAD) <= 512  ? 512
                                                        : 1024);
    }

    /// Allocate memory from pool
    /// Falls back to heap for sizes > MAX_POOLED_SIZE
    static void* allocate(size_t size);

    /// Return memory to pool
    /// size must match the original allocation size
    static void deallocate(void* ptr, size_t size);

    /// Pre-warm pools by allocating blocks upfront
    /// Call at startup to avoid first-allocation latency
    static void prewarm(size_t count_per_class = 256);

    /// Set maximum blocks per size class (0 = unlimited)
    /// When pool is full, deallocate frees to OS instead of pooling
    static void set_max_pool_size(size_t max_blocks_per_class);

    /// Get current max pool size setting
    static size_t get_max_pool_size();

    /// Get pool statistics (for debugging/monitoring)
    struct stats {
        size_t pool_hits = 0;       // Allocations served from pool
        size_t pool_misses = 0;     // Allocations that needed new block
        size_t heap_fallbacks = 0;  // Oversized allocations (heap)
        size_t pool_full_frees = 0; // Blocks freed to OS because pool was full
    };
    static stats get_stats();
    static void reset_stats();

    // Implementation (free lists, statistics) lives in message_pool.cpp.
    // The free lists are moodycamel::ConcurrentQueue instances rather than a
    // hand-rolled Treiber stack: the stack's pop was open to ABA, which handed
    // one block to two owners as soon as two threads both allocated and freed.
};

} // namespace cas

#endif // CAS_MESSAGE_POOL_H
