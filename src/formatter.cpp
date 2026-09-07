#include "mpmc_logger/formatter.hpp"
#include <cstring>

namespace mpmc_logger {

size_t TextFormatter::format(const LogEntry& entry, char* buf,
                              size_t buf_size) noexcept {
    if (buf_size < max_formatted_size()) return 0;

    char* p = buf;

    // Opening bracket for timestamp
    *p++ = '[';

    // Timestamp — uses cached date prefix (only ns suffix changes per-second)
    p += date_fmt_.format(entry.timestamp_ns, p);

    // Close timestamp bracket
    *p++ = ']';
    *p++ = ' ';

    // Log level
    *p++ = '[';
    const char* lvl = level_to_string(entry.level);
    std::memcpy(p, lvl, 5);
    p += 5;
    *p++ = ']';
    *p++ = ' ';

    // Thread ID
    *p++ = '[';
    *p++ = 't';
    *p++ = 'i';
    *p++ = 'd';
    *p++ = ':';

    // Convert thread_id to string
    char tid_buf[12];
    char* tid_end = tid_buf + sizeof(tid_buf);
    char* tid_start = fast_u32_to_str(entry.thread_id, tid_end);
    size_t tid_len = static_cast<size_t>(tid_end - tid_start);
    std::memcpy(p, tid_start, tid_len);
    p += tid_len;

    *p++ = ']';
    *p++ = ' ';

    // Payload
    uint16_t payload_len = entry.payload_len;
    if (payload_len > 112) payload_len = 112;  // Safety clamp
    if (payload_len > 0) {
        std::memcpy(p, entry.payload, payload_len);
        p += payload_len;
    }

    // Newline
    *p++ = '\n';

    return static_cast<size_t>(p - buf);
}

} // namespace mpmc_logger
