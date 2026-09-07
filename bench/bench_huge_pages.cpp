
//
// Allocates the MPMCQueue in a Huge Page (2MB superpage on macOS) and measures
// push/pop throughput compared to standard heap allocation.

#include "mpmc_logger/mpmc_queue.hpp"
#include "mpmc_logger/log_entry.hpp"
#include "mpmc_logger/timestamp.hpp"
#include <benchmark/benchmark.h>
#include <cstring>
#include <thread>
#include <atomic>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/vm_map.h>
#endif

using namespace mpmc_logger;
using HugeQueue = MPMCQueue<LogEntry, 1 << 16>; // 64K slots = ~8MB

// Custom allocator for Huge Pages
template<typename T>
T* allocate_huge_page() {
    size_t size = sizeof(T);
    
#if defined(__APPLE__)
    // Align to 2MB superpage size boundary
    size = (size + (2 * 1024 * 1024) - 1) & ~((2 * 1024 * 1024) - 1);
    
    vm_address_t addr = 0;
    kern_return_t kr = vm_allocate(mach_task_self(), &addr, size, 
                                   VM_FLAGS_ANYWHERE | VM_FLAGS_SUPERPAGE_SIZE_2MB);
    if (kr != KERN_SUCCESS) {
        // Fallback to standard allocation if huge pages are exhausted/unavailable
        std::printf("Warning: Huge page allocation failed (kr=%d). Falling back to standard heap.\n", kr);
        return new T();
    }
    return new (reinterpret_cast<void*>(addr)) T();
#else
    // Default fallback for non-macOS (in a real system, use mmap with MAP_HUGETLB for Linux)
    return new T();
#endif
}

template<typename T>
void deallocate_huge_page(T* ptr) {
#if defined(__APPLE__)
    ptr->~T();
    size_t size = sizeof(T);
    size = (size + (2 * 1024 * 1024) - 1) & ~((2 * 1024 * 1024) - 1);
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(ptr), size);
#else
    delete ptr;
#endif
}

static LogEntry make_huge_bench_entry() {
    LogEntry e;
    e.timestamp_ns = 0;
    e.thread_id    = 1;
    e.level        = LogLevel::LVL_INFO;
    e.payload_len  = 8;
    std::memcpy(e.payload, "hugepage", 8);
    return e;
}

// 1. Standard Heap Queue
static void BM_Queue_StandardHeap(benchmark::State& state) {
    HugeQueue* q = new HugeQueue();
    uint32_t cid = q->register_consumer();
    
    std::atomic<bool> stop{false};
    std::thread drainer([&]() {
        LogEntry out;
        while (!stop.load(std::memory_order_relaxed)) {
            while (q->pop(cid, out)) {}
            q->recompute_and_store_min_tail();
        }
    });

    auto entry = make_huge_bench_entry();
    for (auto _ : state) {
        while (!q->push(entry)) {}
    }

    state.SetItemsProcessed(state.iterations());
    stop.store(true);
    drainer.join();
    delete q;
}
BENCHMARK(BM_Queue_StandardHeap)->UseRealTime();

// 2. Huge Page Queue
static void BM_Queue_HugePage(benchmark::State& state) {
    HugeQueue* q = allocate_huge_page<HugeQueue>();
    uint32_t cid = q->register_consumer();
    
    std::atomic<bool> stop{false};
    std::thread drainer([&]() {
        LogEntry out;
        while (!stop.load(std::memory_order_relaxed)) {
            while (q->pop(cid, out)) {}
            q->recompute_and_store_min_tail();
        }
    });

    auto entry = make_huge_bench_entry();
    for (auto _ : state) {
        while (!q->push(entry)) {}
    }

    state.SetItemsProcessed(state.iterations());
    stop.store(true);
    drainer.join();
    deallocate_huge_page(q);
}
BENCHMARK(BM_Queue_HugePage)->UseRealTime();

BENCHMARK_MAIN();
