
//
// Measures exact CPU cycles and calculates nanoseconds based on CPU frequency
// for three specific operations:
// 1. Push to MPMC Queue (Producer)
// 2. Pop from MPMC Queue (Consumer)
// 3. Write to Log File (mmap_sink)

#include "mpmc_logger/mpmc_queue.hpp"
#include "mpmc_logger/log_entry.hpp"
#include "mpmc_logger/timestamp.hpp"
#include "mpmc_logger/mmap_sink.hpp"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>
#include <algorithm>
#include <span>

using namespace mpmc_logger;
namespace fs = std::filesystem;

// Retrieve CPU frequency for rdtsc cycle conversion
static double get_rdtsc_freq_hz() {
#if defined(__aarch64__) || defined(__arm64__)
    // Apple Silicon cntvct_el0 runs at exactly 24 MHz
    return 24000000.0;
#else
    // For x86_64, a rough estimate is used here (e.g. 3.0 GHz)
    // In a real system, you'd parse /proc/cpuinfo or use cpuid
    return 3000000000.0;
#endif
}

static void print_percentiles(const char* name, std::vector<uint64_t>& cycles, double freq_hz) {
    std::sort(cycles.begin(), cycles.end());
    size_t n = cycles.size();
    if (n == 0) return;

    auto cycles_to_ns = [freq_hz](uint64_t c) { return (static_cast<double>(c) * 1e9) / freq_hz; };

    uint64_t p50_c = cycles[n / 2];
    uint64_t p90_c = cycles[n * 90 / 100];
    uint64_t p99_c = cycles[n * 99 / 100];
    uint64_t p999_c = cycles[n * 999 / 1000];

    std::printf("%-22s | %8.2f | %8.2f | %8.2f | %8.2f\n", 
                name, cycles_to_ns(p50_c), cycles_to_ns(p90_c), cycles_to_ns(p99_c), cycles_to_ns(p999_c));
}

static void bench_rdtsc() {
    double freq_hz = get_rdtsc_freq_hz();
    std::printf("Detected/Assumed rdtsc() frequency: %.2f MHz\n", freq_hz / 1e6);
    
    constexpr int ITERS = 100000;
    
    std::vector<uint64_t> push_cycles(ITERS);
    std::vector<uint64_t> pop_cycles(ITERS);
    std::vector<uint64_t> write_cycles(ITERS);

    static MPMCQueue<LogEntry, 1 << 18> q;
    uint32_t cid = q.register_consumer();
    
    LogEntry entry;
    entry.timestamp_ns = 0;
    entry.thread_id    = 1;
    entry.level        = LogLevel::LVL_INFO;
    entry.payload_len  = 16;
    std::memcpy(entry.payload, "bench_rdtsc_msg!", 16);
    
    // --- Push Benchmark ---
    for (int i = 0; i < ITERS; ++i) {
        uint64_t t0 = rdtsc();
        while (!q.push(entry)) {}
        uint64_t t1 = rdtsc();
        push_cycles[i] = (t1 > t0) ? (t1 - t0) : 0;
    }
    
    // --- Pop Benchmark ---
    LogEntry out;
    for (int i = 0; i < ITERS; ++i) {
        uint64_t t0 = rdtsc();
        while (!q.pop(cid, out)) {}
        if (i % 64 == 0) q.recompute_and_store_min_tail();
        uint64_t t1 = rdtsc();
        pop_cycles[i] = (t1 > t0) ? (t1 - t0) : 0;
    }
    
    // --- Write Benchmark ---
    const std::string test_file = "/tmp/bench_rdtsc_sink.log";
    fs::remove(test_file);
    {
        MMapSink sink(test_file, 16 * 1024 * 1024);
        char log_str[] = "2026-08-25 01:23:45.123456789 [INFO ] [tid:00001] bench_rdtsc_msg!\n";
        size_t len = std::strlen(log_str);
        
        for (int i = 0; i < ITERS; ++i) {
            uint64_t t0 = rdtsc();
            sink.write(std::span<const char>(log_str, len));
            uint64_t t1 = rdtsc();
            write_cycles[i] = (t1 > t0) ? (t1 - t0) : 0;
        }
    }
    fs::remove(test_file);

    std::printf("\n=== RDTSC Microbenchmark Latency (ns) (%d iterations) ===\n", ITERS);
    std::printf("Metric                 |      p50 |      p90 |      p99 |    p99.9\n");
    std::printf("-----------------------|----------|----------|----------|----------\n");
    print_percentiles("Producer Push to Queue", push_cycles, freq_hz);
    print_percentiles("Consumer Pop from Queue", pop_cycles, freq_hz);
    print_percentiles("Consumer Sink Write", write_cycles, freq_hz);
    std::printf("-------------------------------------------------------------------\n");
}

int main() {
    bench_rdtsc();
    return 0;
}
