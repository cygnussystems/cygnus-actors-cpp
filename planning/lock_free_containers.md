# Production-Ready C++ Lock-Free Container Libraries

**The C++ ecosystem offers robust options for lock-free containers, with eight highly production-ready libraries standing out for commercial use.** The most proven choices include Boost.Lockfree for ecosystem integration, Facebook Folly for Meta-scale workloads, and Intel oneTBB for comprehensive parallel programming. Specialized high-performance options like atomic_queue achieve **150 nanosecond round-trip latency** while Rigtorp's libraries power Electronic Arts' Frostbite engine. All libraries surveyed carry permissive licenses (MIT, BSD, Apache 2.0, or Boost) suitable for commercial deployment.

The selection depends on specific needs: ultra-low latency demands atomic_queue, embedded systems benefit from DNedic/lockfree's zero-allocation design, and comprehensive container variety points toward libcds or oneTBB. Recent maintenance activity varies from weekly releases (Folly) to mature stable states (Boost.Lockfree), with all major libraries receiving updates within the past year.

## Battle-tested production libraries

### Boost.Lockfree: The ecosystem standard

**Repository**: https://github.com/boostorg/lockfree  
**License**: Boost Software License 1.0  
**Maintenance**: Active as part of Boost ecosystem (v1.89.0 current)

Boost.Lockfree provides **three core lock-free data structures** tested across millions of deployments. The library implements a multi-producer/multi-consumer queue based on the Michael-Scott algorithm, a multi-producer/multi-consumer stack, and a single-producer/single-consumer queue with **wait-free guarantees** (stronger than lock-free). The SPSC queue, implemented as a ringbuffer, offers significantly better performance than the MPMC variants for single-threaded scenarios.

Configuration flexibility allows compile-time capacity specification via `boost::lockfree::capacity<N>`, fixed-size mode using array-based storage instead of pointers, and custom allocator support including Boost.Interprocess allocators. The library requires **trivial assignment operators and trivial destructors** for contained types, limiting flexibility compared to alternatives like moodycamel. Fixed-size configuration avoids runtime memory allocation, making it suitable for real-time systems, though memory allocation from the OS remains a potential bottleneck on the critical path.

The library's conservative approach prioritizes correctness and portability over raw speed. Being part of Boost ensures regular compatibility updates with new C++ standards and continuous testing across platforms, though feature development proceeds slower than dedicated projects. Choose Boost.Lockfree when your codebase already uses Boost, you need stack or dedicated SPSC implementations, or you prioritize the most conservative well-tested approach over maximum performance.

### Facebook Folly: Meta-scale performance

**Repository**: https://github.com/facebook/folly  
**License**: Apache License 2.0  
**Maintenance**: Highly active with weekly releases (v2025.10.06.00 latest)

Folly delivers production-proven lock-free queues tested at Meta's scale with **thousands of concurrent threads**. The library provides ProducerConsumerQueue for single-producer/single-consumer scenarios and MPMCQueue for multi-producer/multi-consumer workloads. Both use sophisticated cache-aware designs with **hardware_destructive_interference_size** padding to eliminate false sharing between threads.

The MPMCQueue represents cutting-edge research implementation with **formal verification completed using the Coq proof assistant** and published at CPP 2022 conference. This mathematically proven correctness extends to external linearization points for dequeue operations, providing strong consistency guarantees. The ticket-based approach ensures fairness under contention while maintaining high throughput.

Folly's queues integrate seamlessly with Meta's broader utility library, offering complementary features like SmallLocks (1-byte and 1-bit locks) and PackedSyncPtr for memory-efficient synchronization primitives. The library targets C++17 and receives continuous updates from Meta's internal usage, with benchmark results showing competitive or superior performance versus Boost and TBB implementations. Weekly automated releases ensure rapid bug fixes and improvements.

### Intel oneTBB: Comprehensive parallel programming

**Repository**: https://github.com/uxlfoundation/oneTBB  
**License**: Apache License 2.0  
**Maintenance**: Active (186 commits past year, maintained by UXL Foundation)

Intel's Threading Building Blocks, now called oneTBB under UXL Foundation governance, provides **13+ concurrent containers** alongside parallel algorithms and task scheduling. The library delivers concurrent_queue and concurrent_bounded_queue for unbounded and bounded FIFO operations, concurrent_priority_queue with custom comparisons, concurrent_hash_map with fine-grained locking via accessor/const_accessor smart pointers, and concurrent_vector with growth that never relocates existing elements.

The containers use **mixed approaches** balancing lock-free algorithms with fine-grained locking depending on the structure. The concurrent_hash_map employs reader-writer locks per bucket, allowing multiple simultaneous readers or a single writer, while concurrent_vector provides lock-free growth without invalidating iterators. This pragmatic approach prioritizes real-world performance over pure lock-free ideology.

Beyond containers, oneTBB includes a sophisticated **work-stealing scheduler** that automatically balances workload across cores, parallel algorithms (parallel_for, parallel_reduce, parallel_scan, parallel_pipeline, parallel_sort), flow graphs for expressing data dependencies, and scalable memory allocator (tbbmalloc) optimized for concurrent allocation patterns. The library supports C++11/14/17/20 standards across Linux, Windows, macOS, and ARM64 architectures including Windows on ARM. Platform-specific optimizations for NUMA and hybrid CPU architectures arrived in version 2022.2.0.

Choose oneTBB when you need comprehensive concurrent containers beyond queues, require parallel algorithms and task scheduling, or want a complete parallel programming framework rather than individual data structures. The library serves as an industry standard, shipping with Intel toolkits and adopted by major software like Blender and rendering engines.

## Specialized high-performance options

### atomic_queue: Ultra-low latency leader

**Repository**: https://github.com/max0x7ba/atomic_queue  
**License**: MIT  
**Maintenance**: Very active through 2024

atomic_queue achieves **150 nanosecond round-trip latency** through minimalist design philosophy, using bare minimum atomic instructions and explicit false-sharing avoidance. The library provides AtomicQueue for atomic elements, OptimistAtomicQueue for faster busy-waiting variants, AtomicQueue2 for non-atomic elements, and runtime capacity versions (AtomicQueueB variants). Each implementation focuses on different performance characteristics and usage patterns.

The design uses **power-of-2 optimization** replacing modulo operations with bitwise AND for index calculation, swaps element index with cache line index to reduce contention, and maintains fixed-size linear ring buffers for cache-friendly access patterns. Single-producer-single-consumer mode requires only atomic loads/stores without compare-and-swap operations, reducing instruction overhead. The library supports move-only types like `std::unique_ptr<T>` and offers totally-ordered mode with zero cost on Intel x86.

**Battle-tested in low-latency trading** at Charlesworth Research and Marquette Partners, atomic_queue provides value semantics with copy/move on push/pop operations. Memory ordering follows acquire-release semantics with push/try_push as release operations and pop/try_pop as acquire operations, guaranteeing visibility of non-atomic stores across threads. The header-only C++14 implementation supports huge pages for minimizing TLB misses. Extensive benchmarks demonstrate superior performance versus Boost, moodycamel, xenium, and Intel TBB. Available via vcpkg and Conan package managers.

### Rigtorp's libraries: Battle-proven simplicity

**SPSCQueue**: https://github.com/rigtorp/SPSCQueue  
**MPMCQueue**: https://github.com/rigtorp/MPMCQueue  
**License**: MIT  
**Maintenance**: Active with v1.1 (2021) for SPSC, ongoing for MPMC

Erik Rigtorp's focused implementations deliver exceptional performance through simplicity. SPSCQueue achieves **362,723 ops/ms throughput and 133ns round-trip latency** on AMD Ryzen 9 3900X, substantially outperforming boost::lockfree::spsc_queue (209,877 ops/ms, 222ns) and folly::ProducerConsumerQueue (148,818 ops/ms, 147ns). The bounded single-producer single-consumer queue provides wait-free and lock-free guarantees using ring-buffer implementation.

The design caches head and tail indices locally for reduced cache coherency traffic, employs alignment and padding to avoid false sharing, supports arbitrary non-power-of-2 capacities, and enables huge pages allocation on Linux following P0401R3 allocator size feedback. Move-only types receive full support in this C++11-compliant header-only implementation. Academic papers cite the library's correctness and performance characteristics.

MPMCQueue powers **Electronic Arts' Frostbite game engine** across multiple AAA titles and serves low-latency trading infrastructure at Charlesworth Research and Marquette Partners. Based on Orozco et al. (2012) algorithm, the bounded multi-producer multi-consumer queue uses turn-based coordination where each slot has turn tracking to avoid contention. Enqueue operations acquire write tickets from head, wait for turn, then update turn; dequeue operations follow symmetric pattern from tail. This turn-based system prevents slot conflicts while maintaining lock-free properties.

### moodycamel::ConcurrentQueue: Extreme throughput optimization

**Repository**: https://github.com/cameron314/concurrentqueue  
**License**: Dual-licensed (Simplified BSD or Boost Software License 1.0)  
**Maintenance**: Mature and stable (v1.0.4, June 2020)

moodycamel's queue focuses exclusively on maximum throughput, trading strict consistency for performance. The library provides ConcurrentQueue for multi-producer/multi-consumer lock-free operations and BlockingConcurrentQueue adding `wait_dequeue()` and timed wait operations using lightweight semaphores. The **11,400+ GitHub stars** reflect strong community adoption despite less frequent updates indicating maturity rather than abandonment.

**Producer/consumer tokens** provide the key performance optimization: optional `ProducerToken` and `ConsumerToken` objects enable thread-local optimization for significant performance gains. One token per thread is recommended, reusable but not shared between threads simultaneously. Bulk operations via `enqueue_bulk()` and `try_dequeue_bulk()` dramatically outperform individual operations, approaching or exceeding non-concurrent queue performance even under heavy contention.

The queue is **not linearizable by design** - elements from independent producers may not maintain total ordering, sacrificing sequential consistency for speed. The author's benchmarks claim superiority over both `boost::lockfree::queue` and `tbb::concurrent_queue`. Memory management uses contiguous blocks internally instead of linked lists for cache efficiency, with dynamic allocation by default via `enqueue()` and pre-allocation support. The `try_enqueue()` variant never allocates, returning false if insufficient space exists.

Single-header implementation simplifies integration with pure C++11 atomics requiring no assembly. The queue supports move semantics and imposes **no artificial type limitations** unlike Boost's requirements for trivial constructors/destructors. Exception safety guarantees rollback of operations on exception. Customization occurs via traits template parameter controlling block size, initial sizes, and allocation strategies. Choose moodycamel when raw performance is critical, you need blocking queue operations, or bulk operation patterns dominate your usage.

### DNedic/lockfree: Embedded systems champion

**Repository**: https://github.com/DNedic/lockfree  
**License**: MIT  
**Maintenance**: Active (2023-2024)

DNedic/lockfree targets embedded and real-time systems with **zero dynamic allocation** and interrupt-safe operations. The library provides SPSC queue, ring buffer, bipartite buffer, and priority queue with wait-free operations, plus MPMC queue and priority queue variants. Portability spans from 8051 microcontrollers to RISC-V high-performance computing, tested across the full embedded systems spectrum.

The **interrupt-safe design** on embedded platforms allows usage from interrupt service routines without disabling interrupts or risking deadlock. Wait-free SPSC operations guarantee bounded execution time suitable for hard real-time systems. The header-only C++11 implementation integrates via CMake, requiring no external dependencies or runtime support libraries.

Choose DNedic/lockfree for deeply embedded systems, hard real-time requirements, platforms without dynamic memory allocation, or interrupt-driven architectures. The library's minimal resource requirements and guaranteed execution bounds make it ideal for safety-critical systems where predictability matters more than maximum throughput.

## Comprehensive algorithm collections

### libcds: Academic research implementation

**Repository**: https://github.com/khizmax/libcds  
**License**: Boost Software License 1.0  
**Maintenance**: Mature and stable (v2.3.3, December 2019)

libcds implements **over 20 concurrent data structures** drawn from academic research papers, providing the most comprehensive collection available. Queues include BasketQueue (Hoffman, Shalev, Shavit 2007), MSQueue (Michael & Scott classic algorithm), MoirQueue, OptimisticQueue (Ladan-Mozes & Shavit 2008), SegmentedQueue, VyukovMPMCCycleQueue, and FCQueue (flat-combining wrapper). Stacks feature TreiberStack with optional elimination back-off and FCStack flat-combining wrapper.

Maps and sets span MichaelHashMap (lock-free), SplitOrderedList (lock-free resizable hash), StripedMap/Set, CuckooMap/Set, SkipListMap/Set (lock-free), and **FeldmanHashMap/Set with thread-safe bidirectional iterators** using multi-level array hashing. Trees include EllenBinTree (non-blocking binary search tree) and BronsonAVLTreeMap (fine-grained lock-based AVL tree). Priority queues, lazy lists, and ordered lists with thread-safe iterators round out the collection.

The library provides both **intrusive and non-intrusive (STL-like) versions** in separate `cds::intrusive` and `cds::container` namespaces. Safe Memory Reclamation (SMR) algorithms include Hazard Pointers fully refactored in v2.3.0, Dynamic Hazard Pointers adaptive variant, and user-space RCU with multiple implementations (signal-handled, general-purpose, buffered). Flat-combining technique support with configurable wait strategies enables high-performance producer-consumer patterns.

The mostly header-only library requires C++11 minimum (GCC 4.8+, Clang 3.6+, Intel C++ 15+, MSVC 2015+) with small compiled components for SMR core. DCAS (double-width compare-and-swap) support optimizes performance on compatible architectures. Extensive trait-based customization and comprehensive doxygen documentation at http://libcds.sourceforge.net/doc/cds-api/index.html support advanced usage. Available via vcpkg maintained by Microsoft and community.

While the **lack of releases since 2019** suggests minimal active development, the library remains mature and stable. Choose libcds when you need specific academic algorithms, require the widest container variety, or want both intrusive and non-intrusive versions. The implementation quality and comprehensive testing make it suitable for production despite slower maintenance pace.

### Xenium: Configurable reclamation schemes

**Repository**: https://github.com/mpoeter/xenium  
**License**: MIT  
**Maintenance**: Active academic-quality implementation

Xenium distinguishes itself through **comprehensive memory reclamation scheme support**, parameterizing data structures for different reclamation approaches similar to STL allocator customization. The library implements seven distinct reclamation schemes: lock_free_ref_count (Valois 1995, Michael 1995), hazard_pointer (Michael 2004), hazard_eras (Ramalhete & Correia 2017), quiescent_state_based, generic_epoch_based with predefined aliases (new_epoch_based, epoch_based classic, debra), and stamp_it (Pöter & Träff 2018).

Queues span multiple algorithms: michael_scott_queue (unbounded lock-free MPMC from Michael & Scott 1996), ramalhete_queue (fast unbounded lock-free MPMC, Ramalhete 2016), vyukov_bounded_queue (bounded MPMC FIFO, Vyukov 2010), kirsch_kfifo_queue and kirsch_bounded_kfifo_queue (unbounded and bounded MPMC k-FIFO, Kirsch et al. 2013), plus nikolaev_queue and nikolaev_bounded_queue (Nikolaev 2019). Additional structures include harris_michael_list_based_set (lock-free sorted set), harris_michael_hash_map, chase_work_stealing_deque, vyukov_hash_map (concurrent hash map with fine-grained locking), left_right generic implementation, and seqlock.

The **header-only C++17 library** requires no initialization code and supports dynamic thread counts without compile-time specification. Policy-based design allows extensive customization of container behavior and memory management strategies. guard_ptr provides safe access to reclaimed memory but must not move between threads. Focus on C++ memory model correctness ensures proper synchronization semantics across architectures.

Choose Xenium when you need specific reclamation schemes for your application, require modern C++17 features, or want flexibility in memory management strategies. The library suits research projects and applications where reclamation scheme performance characteristics matter. Comprehensive documentation at https://mpoeter.github.io/xenium/ supports implementation.

## Specialized implementations

### Junction: Lock-free hash maps only

**Repository**: https://github.com/preshing/junction  
**License**: Simplified BSD  
**Maintenance**: Mature, less actively maintained

Junction by Jeff Preshing implements **four lock-free concurrent hash map variants**: ConcurrentMap_Crude, ConcurrentMap_Linear, ConcurrentMap_Leapfrog, and ConcurrentMap_Grampa. All member functions provide atomicity with respect to each other, enabling safe concurrent access without mutual exclusion. For Linear, Leapfrog, and Grampa maps, `assign` operations use release semantics and `get` operations use consume semantics, allowing safe passing of non-atomic information between threads via pointers. Crude maps use relaxed operations.

Keys and values must be pointers or pointer-sized integers with **invertible hash functions** ensuring every key has unique hash. The design requires null key reservation and null/redirect value reservation. Iteration cannot occur while concurrently calling assign operations. Memory reclamation requires every thread periodically calling `junction::DefaultQSBR.update` to avoid leaks. Dependencies include the Turf library by the same author.

The specialized focus provides highly optimized hash map implementations without broader container variety. Choose Junction when you specifically need lock-free hash maps and can accommodate the pointer/integer restrictions and manual memory management requirements. The blog post "New Concurrent Hash Maps for C++" by Jeff Preshing details design decisions and performance characteristics.

### sunbains/lockfree-list: Modern doubly-linked list

**Repository**: https://github.com/sunbains/lockfree-list  
**License**: Apache 2.0  
**Maintenance**: Very recent (2024)

This newer entry implements **lock-free doubly-linked list** with STL-compatible bidirectional iterators, filling a gap in the ecosystem. The C++17 header-only implementation achieves **5 million operations per second single-threaded** with cache-friendly design. TLA+ formal verification specifications provide mathematical correctness proofs similar to Folly's approach.

The library targets applications needing bidirectional iteration with concurrent modifications, scenarios where list semantics better fit requirements than queues, and modern C++17 codebases seeking formal verification. **Less battle-tested than established libraries** but comprehensive testing and formal methods increase confidence. Choose when doubly-linked list operations are essential and formal verification matters.

## Performance comparison and selection guidance

The table below compares key characteristics across major production-ready libraries:

| Library | License | Containers | Best For | Latency | Production Use |
|---------|---------|-----------|----------|---------|----------------|
| **atomic_queue** | MIT | 8+ queue variants | Ultra-low latency HPC | 150ns RTT | Trading firms |
| **Rigtorp SPSC** | MIT | SPSC queue | Fastest SPSC | 133ns RTT | General purpose |
| **Rigtorp MPMC** | MIT | MPMC queue | Game engines | Medium | EA Frostbite |
| **Folly** | Apache 2.0 | 2 queues | Meta-scale systems | Low | Meta/Facebook |
| **oneTBB** | Apache 2.0 | 13+ containers | Full parallelism | Medium | Intel ecosystem |
| **Boost.Lockfree** | Boost 1.0 | 3 containers | Boost ecosystem | Medium | Widespread |
| **moodycamel** | BSD/Boost | 2 queues | Bulk throughput | Low | Community |
| **libcds** | Boost 1.0 | 20+ containers | Algorithm variety | Varies | Research/general |
| **Xenium** | MIT | 13+ containers | Custom reclamation | Varies | Academic |
| **DNedic** | MIT | 6 containers | Embedded systems | Varies | Real-time/embedded |
| **Junction** | BSD | 4 hash maps | Hash maps only | Low | Specialized |

Choose based on specific requirements: **ultra-low latency** points to atomic_queue's 150ns performance; **game engines and real-time graphics** benefit from Rigtorp MPMCQueue's proven Frostbite integration; **embedded and safety-critical systems** need DNedic/lockfree's zero-allocation interrupt-safe design; **Meta-scale deployments** leverage Folly's formal verification and production hardening; **comprehensive parallel programming** favors oneTBB's 13+ containers and task scheduling; **maximum SPSC throughput** demands Rigtorp SPSCQueue's benchmark-leading 362,723 ops/ms; **bulk operations** suit moodycamel's optimized bulk enqueue/dequeue; **algorithm research** benefits from libcds's 20+ academic implementations; **custom reclamation strategies** require Xenium's seven reclamation scheme options; **Boost codebases** integrate naturally with Boost.Lockfree; **hash map specialization** points to Junction's four optimized variants.

All libraries carry **permissive licenses suitable for commercial use** including source code modification and proprietary distribution. Maintenance status ranges from highly active weekly releases (Folly v2025.10.06.00) to mature stable implementations (Boost.Lockfree, libcds) receiving periodic updates. Battle-testing in production environments at Meta, Electronic Arts, and financial trading firms validates reliability across diverse workloads.

C++11 minimum requirement applies to most libraries enabling broad compiler support, while modern implementations (Xenium, sunbains/lockfree-list) leverage C++17 features for cleaner interfaces. Header-only designs (atomic_queue, moodycamel, Xenium, DNedic) simplify integration versus libraries requiring build steps. Platform support spans x86-64, ARM64, RISC-V, and embedded systems with architecture-specific optimizations for DCAS and cache line characteristics.

Memory reclamation approaches vary from manual QSBR (Junction) to sophisticated hazard pointers and epoch-based schemes (libcds, Xenium) to zero-allocation designs (DNedic). Understanding reclamation overhead and guarantees matters for real-time systems where predictable latency trumps average-case performance. Wait-free guarantees (Boost SPSC, Rigtorp SPSC, DNedic SPSC) provide stronger progress guarantees than lock-free implementations, essential for hard real-time applications.

Integration considerations include single-header convenience (moodycamel, atomic_queue) versus comprehensive framework integration (oneTBB with parallel algorithms), type restrictions (Boost requiring trivial destructors) versus flexible support (moodycamel accepting any moveable type), and API compatibility with STL conventions versus specialized interfaces. Package manager availability through vcpkg and Conan eases dependency management for atomic_queue, Rigtorp's libraries, and libcds.