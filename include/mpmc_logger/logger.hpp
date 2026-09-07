#pragma once

//
// The user-facing API.  Producer threads call LOG_INFO(...) etc.
// The macro:
//   1. Checks level filter (branch-predicted false)
//   2. Builds a LogEntry on the stack (no allocation)
//   3. Pushes it into the lock-free MPMC queue (~50ns)
//   4. Returns immediately — formatting & I/O happen on the drain thread
//
// The drain thread:
//   1. Pops entries from the queue in a tight loop
//   2. Formats each entry (binary or text)
//   3. Writes formatted bytes into the MMapSink
//   4. On empty queue: spins with _mm_pause(), then adaptive backoff

#include "config.hpp"
#include "log_entry.hpp"
#include "mpmc_queue.hpp"
#include "mmap_sink.hpp"
#include "formatter.hpp"
#include "timestamp.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <thread>

#ifdef __x86_64__
#include <immintrin.h>
#endif

// Platform-specific thread ID caching
#ifdef __linux__
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <pthread.h>
#endif

namespace mpmc_logger {


// gettid() / pthread_threadid_np() are syscalls — cache on first use.

inline uint32_t get_cached_tid() noexcept {
    static thread_local uint32_t tid = 0;
    if (tid == 0) {
#ifdef __linux__
        tid = static_cast<uint32_t>(syscall(SYS_gettid));
#elif defined(__APPLE__)
        uint64_t tid64 = 0;
        pthread_threadid_np(nullptr, &tid64);
        tid = static_cast<uint32_t>(tid64);
#else
        tid = static_cast<uint32_t>(
            std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
    }
    return tid;
}



class Logger {
public:
    
    static Logger& instance() noexcept {
        static Logger inst;
        return inst;
    }

    
    // Must be called once before any logging.  Not thread-safe (call from
    // main thread before spawning workers).
    void init(const LoggerConfig& config);

    
    // Drains remaining entries, flushes sink, joins drain thread.
    void shutdown() noexcept;

    
    __attribute__((always_inline))
    inline bool should_log(LogLevel level) const noexcept {
        return static_cast<uint8_t>(level) >= static_cast<uint8_t>(
            min_level_.load(std::memory_order_relaxed));
    }

    
    void set_level(LogLevel level) noexcept {
        min_level_.store(level, std::memory_order_relaxed);
    }

    LogLevel level() const noexcept {
        return min_level_.load(std::memory_order_relaxed);
    }

    
    // Returns true if enqueued, false if queue full (entry dropped).
    __attribute__((hot))
    inline bool push(const LogEntry& entry) noexcept {
        if (!queue_) return false;
        return queue_->push(entry);
    }

    
    uint64_t entries_pushed() const noexcept {
        return entries_pushed_.load(std::memory_order_relaxed);
    }
    uint64_t entries_dropped() const noexcept {
        return entries_dropped_.load(std::memory_order_relaxed);
    }
    uint64_t entries_drained() const noexcept {
        return entries_drained_.load(std::memory_order_relaxed);
    }

    bool is_running() const noexcept {
        return running_.load(std::memory_order_relaxed);
    }

private:
    Logger() = default;
    ~Logger() { shutdown(); }

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    
    void drain_loop(std::stop_token st);

    
    // We use a fixed 1M-slot queue.  For dynamic sizing we'd need a
    // heap-allocated queue, but fixed size is simpler and HFT-appropriate.
    // Supporting common sizes as template instantiations:
    static constexpr size_t QUEUE_SIZE = 1 << 20;  // 1M slots
    using QueueType = MPMCQueue<LogEntry, QUEUE_SIZE>;

    std::unique_ptr<QueueType> queue_;
    uint32_t consumer_id_ = 0;

    
    std::unique_ptr<MMapSink>       sink_;
    std::unique_ptr<TextFormatter>  text_fmt_;
    OutputFormat                    output_format_ = OutputFormat::BINARY;

    
    std::jthread  drain_thread_;
    std::atomic<bool>    running_{false};
    std::atomic<bool>    initialized_{false};

    
    std::atomic<LogLevel> min_level_{LogLevel::LVL_TRACE};
    size_t drain_batch_size_        = 256;
    size_t spin_count_before_yield_ = 1000;

    
    std::atomic<uint64_t> entries_pushed_{0};
    std::atomic<uint64_t> entries_dropped_{0};
    std::atomic<uint64_t> entries_drained_{0};
};


//
// Usage:
//   LOG_INFO("Order filled: qty=%u price=%u", qty, price);
//
// The macro:
//   1. Branch-predicts the level check as false (most logs are filtered)
//   2. Builds LogEntry on the stack
//   3. snprintf into the fixed 112-byte payload buffer
//   4. Pushes into queue
//
// snprintf is acceptable here because it runs on the PRODUCER thread
// and is bounded to 112 bytes.  For absolute minimum latency, use
// LOG_ENTRY_RAW to push a pre-built LogEntry directly.

#define MPMC_LOG(_lvl, category_val, fmt, ...)                               \
    do {                                                                      \
        auto& _logger = ::mpmc_logger::Logger::instance();                   \
        if (_logger.should_log(_lvl)) {                                      \
            ::mpmc_logger::LogEntry _entry;                                  \
            _entry.timestamp_ns = ::mpmc_logger::wall_clock_ns();            \
            _entry.thread_id    = ::mpmc_logger::get_cached_tid();           \
            _entry.level        = (_lvl);                                    \
            _entry.category     = (category_val);                            \
            int _n = ::snprintf(_entry.payload, sizeof(_entry.payload),      \
                                fmt, ##__VA_ARGS__);                         \
            _entry.payload_len = (_n > 0)                                    \
                ? static_cast<uint16_t>(                                     \
                    (_n < static_cast<int>(sizeof(_entry.payload)))           \
                        ? _n                                                 \
                        : sizeof(_entry.payload))                            \
                : 0;                                                         \
            _logger.push(_entry);                                            \
        }                                                                     \
    } while (0)

#define LOG_TRACE(fmt, ...)  MPMC_LOG(::mpmc_logger::LogLevel::LVL_TRACE, 0, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...)  MPMC_LOG(::mpmc_logger::LogLevel::LVL_DEBUG, 0, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)   MPMC_LOG(::mpmc_logger::LogLevel::LVL_INFO,  0, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)   MPMC_LOG(::mpmc_logger::LogLevel::LVL_WARN,  0, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...)  MPMC_LOG(::mpmc_logger::LogLevel::LVL_ERROR, 0, fmt, ##__VA_ARGS__)
#define LOG_FATAL(fmt, ...)  MPMC_LOG(::mpmc_logger::LogLevel::LVL_FATAL, 0, fmt, ##__VA_ARGS__)

// Category-aware variants
#define LOG_INFO_CAT(cat, fmt, ...)  MPMC_LOG(::mpmc_logger::LogLevel::LVL_INFO, cat, fmt, ##__VA_ARGS__)
#define LOG_WARN_CAT(cat, fmt, ...)  MPMC_LOG(::mpmc_logger::LogLevel::LVL_WARN, cat, fmt, ##__VA_ARGS__)

// Raw push — for when you've pre-built a LogEntry and want zero formatting
#define LOG_ENTRY_RAW(entry)                                                  \
    do {                                                                      \
        ::mpmc_logger::Logger::instance().push(entry);                       \
    } while (0)

} // namespace mpmc_logger
