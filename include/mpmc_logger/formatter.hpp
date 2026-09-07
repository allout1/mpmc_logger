#pragma once

//
// Two output modes:
//   BinaryFormatter — writes raw LogEntry bytes (trivial memcpy, max throughput)
//   TextFormatter   — converts LogEntry → human-readable line
//
// Both formatters operate OFF the hot path, on the drain thread only.

#include "log_entry.hpp"
#include "timestamp.hpp"
#include <cstddef>

namespace mpmc_logger {


// Writes the raw 128-byte LogEntry directly.  Maximum throughput.
// Requires the `log_reader` tool to decode offline.

class BinaryFormatter {
public:
    // Format entry into buf.  Returns bytes written (always 128).
    static size_t format(const LogEntry& entry, char* buf, size_t buf_size) noexcept {
        if (buf_size < sizeof(LogEntry)) return 0;
        fast_copy_128(buf, &entry);
        return sizeof(LogEntry);
    }

    static constexpr size_t max_formatted_size() noexcept {
        return sizeof(LogEntry);
    }
};


// Output format:
//   [2026-08-24 22:15:03.123456789] [INFO ] [tid:12345] message text here\n
//
// Uses CachedDateFormatter for fast timestamp rendering.

class TextFormatter {
public:
    // Format entry into buf.  Returns bytes written.
    // buf_size should be at least max_formatted_size().
    size_t format(const LogEntry& entry, char* buf, size_t buf_size) noexcept;

    // Maximum possible formatted line length:
    //   [29-char timestamp] + brackets/spaces + level + tid + payload + newline
    //   = 1 + 29 + 2 + 1 + 5 + 2 + 1 + 4 + 10 + 2 + 112 + 1 = ~170
    static constexpr size_t max_formatted_size() noexcept {
        return 256;  // Generous upper bound
    }

private:
    CachedDateFormatter date_fmt_;
};


// Writes digits of `val` into `buf` (right-aligned, no NUL terminator).
// Returns pointer to the first digit written.

inline char* fast_u32_to_str(uint32_t val, char* buf_end) noexcept {
    if (val == 0) {
        *--buf_end = '0';
        return buf_end;
    }
    while (val > 0) {
        *--buf_end = '0' + static_cast<char>(val % 10);
        val /= 10;
    }
    return buf_end;
}

} // namespace mpmc_logger
