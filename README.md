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


## Integration & Usage

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
