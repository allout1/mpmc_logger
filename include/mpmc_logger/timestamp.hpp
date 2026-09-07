#pragma once

#include <cstdint>
#include <ctime>
#include <cstring>

#ifdef __x86_64__
#include <x86intrin.h>
#endif

namespace mpmc_logger {


//
// Uses clock_gettime(CLOCK_MONOTONIC) for a portable, monotonic nanosecond
// timestamp.  On x86-64 with invariant TSC, we also provide an rdtsc-based
// alternative that avoids the syscall entirely after calibration.

__attribute__((always_inline))
inline uint64_t now_ns() noexcept {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL
         + static_cast<uint64_t>(ts.tv_nsec);
}

// Wall-clock timestamp for log output (CLOCK_REALTIME)
__attribute__((always_inline))
inline uint64_t wall_clock_ns() noexcept {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL
         + static_cast<uint64_t>(ts.tv_nsec);
}

#ifdef __x86_64__
// Raw TSC read — cheapest possible timestamp on x86-64 (~20 cycles)
__attribute__((always_inline))
inline uint64_t rdtsc() noexcept {
    return __rdtsc();
}
#elif defined(__aarch64__) || defined(__arm64__)
// Virtual timer count on ARM64 (ticks at 24 MHz on Apple Silicon)
__attribute__((always_inline))
inline uint64_t rdtsc() noexcept {
    uint64_t val;
    asm volatile("mrs %0, cntvct_el0" : "=r" (val));
    return val;
}
#endif


//
// Calibrated at startup: measure TSC ticks per nanosecond by comparing
// rdtsc delta with clock_gettime delta over a short spin.
// Declared here, defined in timestamp.cpp.

struct TscCalibration {
    double ticks_per_ns;
    bool   available;       // false if TSC is not invariant or not x86
};

// Call once at startup.  Thread-safe (idempotent).
TscCalibration calibrate_tsc();

// Convert TSC ticks to nanoseconds using calibration
inline uint64_t tsc_to_ns(uint64_t ticks, const TscCalibration& cal) noexcept {
    return static_cast<uint64_t>(static_cast<double>(ticks) / cal.ticks_per_ns);
}


//
// For text-mode output, we cache the formatted date prefix so that within
// the same second we only format the nanosecond suffix.
//
// Output: "2026-08-24 22:15:03.123456789"

struct CachedDateFormatter {
    time_t   cached_sec = 0;
    char     date_prefix[20];   // "2026-08-24 22:15:03"  (19 chars + NUL)

    // Format a wall-clock nanosecond timestamp into buf.
    // Returns number of bytes written (always 29).
    size_t format(uint64_t wall_ns, char* buf) noexcept {
        time_t sec = static_cast<time_t>(wall_ns / 1'000'000'000ULL);
        uint32_t ns_part = static_cast<uint32_t>(wall_ns % 1'000'000'000ULL);

        if (sec != cached_sec) {
            cached_sec = sec;
            struct tm tm_buf;
            localtime_r(&sec, &tm_buf);
            strftime(date_prefix, sizeof(date_prefix), "%Y-%m-%d %H:%M:%S", &tm_buf);
        }

        // Copy cached prefix (19 bytes)
        std::memcpy(buf, date_prefix, 19);
        buf[19] = '.';

        // Format nanosecond suffix using fast digit extraction
        // "123456789" — 9 digits, zero-padded
        for (int i = 8; i >= 0; --i) {
            buf[20 + i] = '0' + (ns_part % 10);
            ns_part /= 10;
        }

        return 29; // "2026-08-24 22:15:03.123456789"
    }
};

} // namespace mpmc_logger
