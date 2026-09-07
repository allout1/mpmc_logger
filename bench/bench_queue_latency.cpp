
//
// Measures per-push latency using rdtsc (or clock_gettime).
// Produces a latency histogram with p50, p90, p99, p99.9, p99.99, max.

#include "mpmc_logger/mpmc_queue.hpp"
#include "mpmc_logger/log_entry.hpp"
#include "mpmc_logger/timestamp.hpp"
#include <benchmark/benchmark.h>
#include <thread>
#include <vector>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <atomic>

using namespace mpmc_logger;

using LatencyQueue = MPMCQueue<LogEntry, 1 << 20>;



struct LatencyHistogram {
    std::vector<uint64_t> samples;

    void add(uint64_t ns) { samples.push_back(ns); }

    void report() {
        if (samples.empty()) return;
        std::sort(samples.begin(), samples.end());
        size_t n = samples.size();

        std::printf("\n=== Latency Histogram (%zu samples) ===\n", n);
        std::printf("  p50:    %8llu ns\n", samples[n * 50 / 100]);
        std::printf("  p90:    %8llu ns\n", samples[n * 90 / 100]);
        std::printf("  p99:    %8llu ns\n", samples[n * 99 / 100]);
        std::printf("  p99.9:  %8llu ns\n", samples[n * 999 / 1000]);
        std::printf("  p99.99: %8llu ns\n", samples[n * 9999 / 10000]);
        std::printf("  max:    %8llu ns\n", samples[n - 1]);
        std::printf("  min:    %8llu ns\n", samples[0]);
        std::printf("  avg:    %8llu ns\n",
            [&]() -> uint64_t {
                uint64_t sum = 0;
                for (auto s : samples) sum += s;
                return sum / n;
            }());
    }
};



static void BM_PushLatency(benchmark::State& state) {
    LatencyQueue q;
    uint32_t cid = q.register_consumer();

    // Drain thread
    std::atomic<bool> stop{false};
    std::thread drainer([&]() {
        LogEntry out;
        while (!stop.load(std::memory_order_relaxed)) {
            while (q.pop(cid, out)) {}
            q.recompute_and_store_min_tail();
        }
    });

    LogEntry entry;
    entry.timestamp_ns = 0;
    entry.thread_id    = 1;
    entry.level        = LogLevel::LVL_INFO;
    entry.payload_len  = 8;
    std::memcpy(entry.payload, "latency!", 8);

    LatencyHistogram hist;

    for (auto _ : state) {
        uint64_t start = now_ns();
        q.push(entry);
        uint64_t end = now_ns();
        uint64_t delta = end - start;
        hist.add(delta);
        benchmark::DoNotOptimize(delta);
    }

    // Report percentiles
    hist.report();

    state.SetItemsProcessed(state.iterations());
    stop.store(true);
    drainer.join();
}
BENCHMARK(BM_PushLatency)
    ->Iterations(1000000)
    ->UseRealTime();



#include <mutex>
#include <queue>

static void BM_MutexPushLatency(benchmark::State& state) {
    std::mutex mtx;
    std::queue<LogEntry> mq;

    LogEntry entry;
    entry.timestamp_ns = 0;
    entry.thread_id    = 1;
    entry.level        = LogLevel::LVL_INFO;
    entry.payload_len  = 8;
    std::memcpy(entry.payload, "mutexlat", 8);

    LatencyHistogram hist;

    for (auto _ : state) {
        uint64_t start = now_ns();
        {
            std::lock_guard<std::mutex> lk(mtx);
            mq.push(entry);
        }
        uint64_t end = now_ns();
        hist.add(end - start);

        // Pop to keep memory bounded
        {
            std::lock_guard<std::mutex> lk(mtx);
            if (!mq.empty()) mq.pop();
        }
    }

    std::printf("\n--- Mutex Baseline ---");
    hist.report();

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MutexPushLatency)
    ->Iterations(1000000)
    ->UseRealTime();

BENCHMARK_MAIN();
