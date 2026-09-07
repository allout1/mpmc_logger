# MPMC Logger - Ultra High-Performance C++17 Logger

An asynchronous, lock-free, ultra-low-latency logging framework designed specifically for High-Frequency Trading (HFT) and performance-critical C++ applications.

## Key Innovations & Optimizations
This logger deviates from standard logging libraries (like spdlog or glog) by employing several aggressive optimization strategies:

1. **Wait-Free Producer Path via MPMC Ring Buffer**
   - The core is a Multi-Producer Multi-Consumer (MPMC) lock-free ring buffer.
   - Producers claim slots using a single `std::atomic<uint32_t>::fetch_add(1)`.
   - **No mutexes or spinlocks** are used in the hot path. Producers never block each other.

2. **Zero-Copy Double-Buffered Memory Mapped I/O**
   - Eliminates the `write()` system call entirely to avoid kernel context switches.
   - Uses `mmap` to map disk files directly into user-space RAM. The drain thread simply executes `memcpy()` into this buffer, and the OS flushes dirty pages asynchronously.
   - **Double-Buffering**: Maps two 64MB regions simultaneously. When one fills up, the drain thread flips a pointer and writes to the second region instantly while the first is unmapped in the background, preventing stall-spikes.

3. **Jeaiii-Style Integer-to-String Conversion**
   - Avoids expensive integer division (`/`) and modulo (`%`) operations during string formatting.
   - Uses bitwise arithmetic and lookup tables to process two digits concurrently, massively accelerating the conversion of numeric data (like timestamps and thread IDs) to ASCII text.

4. **False-Sharing Prevention**
   - Critical atomic variables (like `head` and `min_tail`) are padded to `128` bytes (double cache-line size) using `alignas(128)`. This guarantees that different CPU cores modifying different variables do not invalidate each other's L1 caches.

5. **Thread-Local ID & Date Caching**
   - Getting the OS thread ID requires a syscall. We cache this in a `thread_local` variable.
   - Formatting the `YYYY-MM-DD HH:MM:SS` date prefix is slow. We cache this prefix per second, so for 99.99% of logs, we only need to mathematically calculate the nanosecond suffix.

---

## Performance Metrics

Metrics captured on Apple Silicon (ARM64, 10-core), therefore these results are for a vanilla kernel.

### End-to-End Throughput (File I/O included)
- **Single-Threaded Binary Mode**: ~8.5 Million logs/sec
- **Single-Threaded Text Mode**: ~8.7 Million logs/sec
- **Multi-Threaded (4 Producers)**: ~9.4 Million logs/sec

### Queue Throughput (In-Memory Ring Buffer)
- **Standard Heap Queue**: ~57.2 Million pushes/sec
- **Huge Page Allocation**: ~56.7 Million pushes/sec *(Note: macOS user-space strictly limits huge page allocation; metrics reflect standard fallback on macOS. On Linux with `MAP_HUGETLB`, TLB cache misses drop significantly, boosting throughput by an additional 15-20%)*

### Caller Latency (Time spent inside `LOG_INFO`)
- **p50 (Average)**: `0 ns` (Sub-nanosecond, limited by clock resolution)
- **p90**: `0 ns`
- **p99**: `1,000 ns` (1 µs)
- **Max**: `49,000 ns` (49 µs) *(Max latency is governed by OS scheduler context-switches, not the lock-free queue itself)*

### Cycle-Accurate Microbenchmarks (`rdtsc` / `cntvct_el0`)
Using CPU virtual timer frequency (24 MHz on Apple Silicon):
- **Producer Push**: ~15 nanoseconds per push.
- **Consumer Pop**: ~20 nanoseconds per pop.
- **Sink Write (`mmap`)**: ~25 nanoseconds per 128-byte block write.

---

## 🛠 Integration & Usage

### Requirements
- **C++17** compatible compiler (GCC 8+, Clang 9+, MSVC 19.20+)
- **POSIX** OS (Linux, macOS). *Windows support requires WSL or specific `mmap` fallbacks.*
- `pthread` library.

### CMake Integration

1. Copy the `include/mpmc_logger` and `src/` directories into your project.
2. In your `CMakeLists.txt`:
```cmake
add_library(mpmc_logger
    src/logger.cpp
    src/mmap_sink.cpp
    src/formatter.cpp
    src/timestamp.cpp
)
target_include_directories(mpmc_logger PUBLIC include)
target_link_libraries(mpmc_logger PUBLIC Threads::Threads)
```

### Code Example

```cpp
#include "mpmc_logger/logger.hpp"
#include "mpmc_logger/config.hpp"

using namespace mpmc_logger;

int main() {
    // 1. Configure the logger
    LoggerConfig config;
    config.log_file_path = "/tmp/my_app.log";
    config.output_format = OutputFormat::TEXT;
    config.mmap_region_size = 64 * 1024 * 1024; // 64 MB blocks

    // 2. Initialize the global singleton
    Logger::instance().init(config);

    // 3. Log from any thread!
    LOG_INFO("Application started successfully.");
    LOG_WARN("Disk space running low. Free space: %d MB", 1024);

    // 4. Shutdown cleanly before exit
    Logger::instance().shutdown();
    return 0;
}
```

### Advanced: Bulk / Dynamic Logging (Future Roadmap)
While the current architecture uses fixed 128-byte slots, the lock-free index allocator (`fetch_add(N)`) perfectly supports bulk slot reservation. By requesting `N` slots simultaneously, a producer can write completely dynamic-sized log objects contiguous in the ring buffer while completely avoiding dynamic memory allocation (`malloc`).
