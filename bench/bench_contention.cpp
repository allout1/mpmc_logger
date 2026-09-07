
//
// Measures how throughput and tail latency scale with producer/consumer count.

#include "mpmc_logger/mpmc_queue.hpp"
#include "mpmc_logger/log_entry.hpp"
#include "mpmc_logger/timestamp.hpp"
#include <benchmark/benchmark.h>
#include <thread>
#include <vector>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <algorithm>

using namespace mpmc_logger;

using ContQueue = MPMCQueue<LogEntry, 1 << 20>;



static void BM_ProducerScaling(benchmark::State& state) {
    const int num_producers = static_cast<int>(state.range(0));
    constexpr int OPS_PER_PRODUCER = 500000;

    ContQueue q;
    uint32_t cid = q.register_consumer();

    std::atomic<bool> stop{false};
    std::atomic<int64_t> total_pushed{0};

    // Consumer
    std::thread consumer([&]() {
        LogEntry out;
        while (!stop.load(std::memory_order_relaxed)) {
            while (q.pop(cid, out)) {}
            q.recompute_and_store_min_tail();
        }
        // Final drain
        LogEntry out2;
        while (q.pop(cid, out2)) {}
    });

    for (auto _ : state) {
        total_pushed.store(0);

        std::vector<std::thread> producers;
        for (int p = 0; p < num_producers; ++p) {
            producers.emplace_back([&q, &total_pushed]() {
                LogEntry entry;
                entry.timestamp_ns = 0;
                entry.thread_id    = 1;
                entry.level        = LogLevel::LVL_INFO;
                entry.payload_len  = 8;
                std::memcpy(entry.payload, "scaling!", 8);

                for (int i = 0; i < OPS_PER_PRODUCER; ++i) {
                    while (!q.push(entry)) {
                        std::this_thread::yield();
                    }
                    total_pushed.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        for (auto& t : producers) t.join();
    }

    state.SetItemsProcessed(
        static_cast<int64_t>(state.iterations()) * num_producers * OPS_PER_PRODUCER);
    state.counters["producers"] = num_producers;

    stop.store(true);
    consumer.join();
}
BENCHMARK(BM_ProducerScaling)
    ->Arg(1)->Arg(2)->Arg(4)->Arg(8)->Arg(12)->Arg(16)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);



static void BM_ConsumerScaling(benchmark::State& state) {
    const int num_consumers = static_cast<int>(state.range(0));
    constexpr int NUM_PRODUCERS     = 4;
    constexpr int OPS_PER_PRODUCER  = 200000;
    constexpr int TOTAL = NUM_PRODUCERS * OPS_PER_PRODUCER;

    for (auto _ : state) {
        ContQueue q;

        std::vector<uint32_t> cids;
        for (int i = 0; i < num_consumers; ++i) {
            cids.push_back(q.register_consumer());
        }

        std::atomic<bool> producers_done{false};

        // Producers
        std::vector<std::thread> producers;
        for (int p = 0; p < NUM_PRODUCERS; ++p) {
            producers.emplace_back([&q]() {
                LogEntry entry;
                entry.timestamp_ns = 0;
                entry.thread_id    = 1;
                entry.level        = LogLevel::LVL_INFO;
                entry.payload_len  = 4;
                std::memcpy(entry.payload, "cons", 4);

                for (int i = 0; i < OPS_PER_PRODUCER; ++i) {
                    while (!q.push(entry)) {
                        std::this_thread::yield();
                    }
                }
            });
        }

        // Consumers
        std::vector<std::thread> consumers;
        for (int c = 0; c < num_consumers; ++c) {
            consumers.emplace_back([&q, cid = cids[c], &producers_done]() {
                LogEntry out;
                int count = 0;
                while (count < TOTAL) {
                    if (q.pop(cid, out)) {
                        ++count;
                    } else {
                        if (producers_done.load(std::memory_order_relaxed) &&
                            count >= TOTAL)
                            break;
                        std::this_thread::yield();
                    }
                }
            });
        }

        // Helper to refresh min_tail
        std::thread helper([&q, &producers_done, num_consumers, &cids, TOTAL]() {
            while (!producers_done.load(std::memory_order_relaxed)) {
                q.recompute_and_store_min_tail();
                std::this_thread::yield();
            }
            // Keep going until consumers finish
            for (int i = 0; i < 1000; ++i) {
                q.recompute_and_store_min_tail();
                std::this_thread::yield();
            }
        });

        for (auto& t : producers) t.join();
        producers_done.store(true);
        for (auto& t : consumers) t.join();
        helper.join();
    }

    state.SetItemsProcessed(
        static_cast<int64_t>(state.iterations()) * TOTAL * num_consumers);
    state.counters["consumers"] = num_consumers;
}
BENCHMARK(BM_ConsumerScaling)
    ->Arg(1)->Arg(2)->Arg(4)->Arg(8)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();
