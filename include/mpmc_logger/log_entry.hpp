#pragma once

#include <cstdint>
#include <cstring>

#ifdef __AVX2__
#include <immintrin.h>
#endif

namespace mpmc_logger {



enum class LogLevel : uint8_t {
    LVL_TRACE = 0,
    LVL_DEBUG = 1,
    LVL_INFO  = 2,
    LVL_WARN  = 3,
    LVL_ERROR = 4,
    LVL_FATAL = 5,
    LVL_OFF   = 6   // Used for filtering: set level to OFF to suppress all logs
};

inline const char* level_to_string(LogLevel level) noexcept {
    static constexpr const char* names[] = {
        "TRACE", "DEBUG", "INFO ", "WARN ", "ERROR", "FATAL", "OFF  "
    };
    const auto idx = static_cast<uint8_t>(level);
    return (idx <= 6) ? names[idx] : "?????";
}


//
// Fixed 128-byte POD struct. No heap allocation, no virtual calls, no
// constructors that touch memory.  This is the unit of data that flows
// through the lock-free queue.
//
// Layout (128 bytes total):
//   [0..7]    timestamp_ns   (8)
//   [8..11]   thread_id      (4)
//   [12]      level          (1)
//   [13]      category       (1)
//   [14..15]  payload_len    (2)
//   [16..127] payload        (112)
//   ─────────────────────────────
//                            128

struct alignas(64) LogEntry {
    uint64_t timestamp_ns;          // Nanosecond timestamp (CLOCK_MONOTONIC or TSC)
    uint32_t thread_id;             // Cached thread ID (gettid / pthread_self)
    LogLevel level;                 // Log severity
    uint8_t  category;              // User-defined category tag (0 = default)
    uint16_t payload_len;           // Actual bytes used in payload[] (0..112)
    char     payload[112];          // Inline fixed buffer — no malloc ever

    // Zero-init for safety in debug builds; optimized away in release
    LogEntry() noexcept
        : timestamp_ns(0)
        , thread_id(0)
        , level(LogLevel::LVL_INFO)
        , category(0)
        , payload_len(0)
    {
        // Don't zero payload — it's write-before-read
    }
};

static_assert(sizeof(LogEntry) == 128,
    "LogEntry must be exactly 128 bytes for AVX2 copy and cache alignment");
static_assert(alignof(LogEntry) == 64,
    "LogEntry must be 64-byte aligned for cache line isolation");


//
// 4x 256-bit stores.  Replaces memcpy for the exact sizeof(LogEntry).
// Falls back to memcpy on non-AVX2 platforms.

__attribute__((always_inline))
inline void fast_copy_128(void* __restrict dst,
                          const void* __restrict src) noexcept {
#if defined(__AVX2__)
    __m256i y0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(
                     static_cast<const char*>(src)));
    __m256i y1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(
                     static_cast<const char*>(src) + 32));
    __m256i y2 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(
                     static_cast<const char*>(src) + 64));
    __m256i y3 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(
                     static_cast<const char*>(src) + 96));

    _mm256_storeu_si256(reinterpret_cast<__m256i*>(
                            static_cast<char*>(dst)),      y0);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(
                            static_cast<char*>(dst) + 32), y1);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(
                            static_cast<char*>(dst) + 64), y2);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(
                            static_cast<char*>(dst) + 96), y3);
#else
    std::memcpy(dst, src, 128);
#endif
}

} // namespace mpmc_logger
