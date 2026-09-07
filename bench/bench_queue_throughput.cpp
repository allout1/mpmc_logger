
//
// Measures raw push/pop throughput with varying producer counts.
// N producer threads push, 1 consumer thread pops.

#include "mpmc_logger/mpmc_queue.hpp"
#include "mpmc_logger/log_entry.hpp"
#include <benchmark/benchmark.h>
#include <thread>
#include <vector>
#include <atomic>
#include <cstring>

using namespace mpmc_logger;

using BenchQueue = MPMCQueue<LogEntry, 1 << 20>;  // 1M slots

static LogEntry make_bench_entry() {
    LogEntry e;
    e.timestamp_ns = 123456789;
    e.thread_id    = 1;
    e.level        = LogLevel::LVL_INFO;
    e.payload_len  = 16;
    std::memcpy(e.payload, "benchmark_entry!", 16);
    return e;
}



static void BM_QueuePush_1P(benchmark::State& state) {
    BenchQueue q;
    uint32_t cid = q.register_consumer();
    auto entry = make_bench_entry();

    // Drain thread
    std::atomic<bool> stop{false};
    std::thread drainer([&]() {
        LogEntry out;
        while (!stop.load(std::memory_order_relaxed)) {
            while (q.pop(cid, out)) {}
            q.recompute_and_store_min_tail();
        }
    });

    for (auto _ : state) {
        if (!q.push(entry)) {
            // Queue full — shouldn't happen with drainer, but handle it
            benchmark::DoNotOptimize(entry);
        }
    }

    state.SetItemsProcessed(state.iterations());
    stop.store(true, std::memory_order_relaxed);
    drainer.join();
}
BENCHMARK(BM_QueuePush_1P)->UseRealTime();



static void BM_QueuePush_NP(benchmark::State& state) {
    static BenchQueue q;
    static std::atomic<bool> stop{false};
    static std::atomic<uint32_t> ready_producers{0};
    static uint32_t cid;

    const int num_threads = static_cast<int>(state.range(0));

    if (state.thread_index() == 0) {
        // First thread: setup
        new (&q) BenchQueue();
        stop.store(false);
        ready_producers.store(0);
        cid = q.register_consumer();
    }

    // Drain thread (only from thread 0)
    std::thread drainer;
    if (state.thread_index() == 0) {
        drainer = std::thread([&]() {
            LogEntry out;
            while (!stop.load(std::memory_order_relaxed)) {
                while (q.pop(cid, out)) {}
                q.recompute_and_store_min_tail();
            }
        });
    }

    auto entry = make_bench_entry();

    for (auto _ : state) {
        while (!q.push(entry)) {
            std::this_thread::yield();
        }
    }

    state.SetItemsProcessed(state.iterations());

    if (state.thread_index() == 0) {
        stop.store(true);
        if (drainer.joinable()) drainer.join();
    }
}
BENCHMARK(BM_QueuePush_NP)
    ->Arg(1)->Arg(2)->Arg(4)->Arg(8)->Arg(16)
    ->UseRealTime()
    ->MeasureProcessCPUTime();



static void BM_QueuePop(benchmark::State& state) {
    BenchQueue q;
    uint32_t cid = q.register_consumer();

    // Pre-fill queue
    auto entry = make_bench_entry();
    for (size_t i = 0; i < (1 << 20) - 1; ++i) {
        q.push(entry);
    }

    LogEntry out;
    for (auto _ : state) {
        if (!q.pop(cid, out)) {
            state.SkipWithError("Queue empty unexpectedly");
            break;
        }
        benchmark::DoNotOptimize(out);
        // Re-push to keep queue non-empty
        q.recompute_and_store_min_tail();
        q.push(entry);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_QueuePop)->UseRealTime();

BENCHMARK_MAIN();
