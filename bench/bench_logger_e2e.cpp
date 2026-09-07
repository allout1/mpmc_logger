
//
// Measures full pipeline: LOG_INFO() → queue → drain → format → mmap write.
// Reports caller-side latency (time spent in LOG_INFO macro).

#include "mpmc_logger/logger.hpp"
#include "mpmc_logger/config.hpp"
#include "mpmc_logger/timestamp.hpp"
#include <benchmark/benchmark.h>
#include <filesystem>
#include <thread>
#include <vector>
#include <cstdio>
#include <algorithm>

using namespace mpmc_logger;
namespace fs = std::filesystem;

static const std::string BENCH_DIR = "/tmp/mpmc_logger_bench";



static void BM_LoggerE2E_Binary(benchmark::State& state) {
    fs::remove_all(BENCH_DIR);
    fs::create_directories(BENCH_DIR);

    LoggerConfig config;
    config.log_file_path    = BENCH_DIR + "/bench_binary.log";
    config.output_format    = OutputFormat::BINARY;
    config.mmap_region_size = 64 * 1024 * 1024;

    Logger::instance().init(config);

    for (auto _ : state) {
        LOG_INFO("bench entry seq=%d price=%d qty=%d", 42, 10050, 100);
    }

    state.SetItemsProcessed(state.iterations());
    Logger::instance().shutdown();
    fs::remove_all(BENCH_DIR);
}
BENCHMARK(BM_LoggerE2E_Binary)
    ->Iterations(5000000)
    ->UseRealTime();



static void BM_LoggerE2E_Text(benchmark::State& state) {
    fs::remove_all(BENCH_DIR);
    fs::create_directories(BENCH_DIR);

    LoggerConfig config;
    config.log_file_path    = BENCH_DIR + "/bench_text.log";
    config.output_format    = OutputFormat::TEXT;
    config.mmap_region_size = 64 * 1024 * 1024;

    Logger::instance().init(config);

    for (auto _ : state) {
        LOG_INFO("bench entry seq=%d price=%d qty=%d", 42, 10050, 100);
    }

    state.SetItemsProcessed(state.iterations());
    Logger::instance().shutdown();
    fs::remove_all(BENCH_DIR);
}
BENCHMARK(BM_LoggerE2E_Text)
    ->Iterations(5000000)
    ->UseRealTime();



static void BM_LoggerE2E_4Threads(benchmark::State& state) {
    if (state.thread_index() == 0) {
        fs::remove_all(BENCH_DIR);
        fs::create_directories(BENCH_DIR);

        LoggerConfig config;
        config.log_file_path    = BENCH_DIR + "/bench_mt.log";
        config.output_format    = OutputFormat::BINARY;
        config.mmap_region_size = 64 * 1024 * 1024;

        Logger::instance().init(config);
    }

    for (auto _ : state) {
        LOG_INFO("mt bench tid=%d val=%d", 42, 99);
    }

    state.SetItemsProcessed(state.iterations());

    if (state.thread_index() == 0) {
        // Wait for other threads
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        Logger::instance().shutdown();
        fs::remove_all(BENCH_DIR);
    }
}
BENCHMARK(BM_LoggerE2E_4Threads)
    ->Threads(4)
    ->Iterations(1000000)
    ->UseRealTime();



static void BM_LoggerCallerLatency(benchmark::State& state) {
    fs::remove_all(BENCH_DIR);
    fs::create_directories(BENCH_DIR);

    LoggerConfig config;
    config.log_file_path    = BENCH_DIR + "/bench_latency.log";
    config.output_format    = OutputFormat::BINARY;
    config.mmap_region_size = 64 * 1024 * 1024;

    Logger::instance().init(config);

    std::vector<uint64_t> latencies;
    latencies.reserve(state.max_iterations);

    for (auto _ : state) {
        uint64_t start = now_ns();
        LOG_INFO("latency test value=%d", 42);
        uint64_t end = now_ns();
        latencies.push_back(end - start);
    }

    // Print histogram
    std::sort(latencies.begin(), latencies.end());
    size_t n = latencies.size();
    if (n > 0) {
        std::printf("\n=== Logger Caller Latency (%zu samples) ===\n", n);
        std::printf("  p50:    %8llu ns\n", latencies[n * 50 / 100]);
        std::printf("  p90:    %8llu ns\n", latencies[n * 90 / 100]);
        std::printf("  p99:    %8llu ns\n", latencies[n * 99 / 100]);
        std::printf("  p99.9:  %8llu ns\n", latencies[n * 999 / 1000]);
        std::printf("  max:    %8llu ns\n", latencies[n - 1]);
    }

    state.SetItemsProcessed(state.iterations());
    Logger::instance().shutdown();
    fs::remove_all(BENCH_DIR);
}
BENCHMARK(BM_LoggerCallerLatency)
    ->Iterations(1000000)
    ->UseRealTime();

BENCHMARK_MAIN();
