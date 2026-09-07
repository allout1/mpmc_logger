// ─── Log Statistics Tool ────────────────────────────────────────────────────
//
// Reads binary log files and computes statistics:
//   - Total entries, entries per level
//   - Entries per thread
//   - Time span and entries/sec
//   - Payload length distribution
//
// Usage:
//   ./log_stats <logfile>

#include "mpmc_logger/log_entry.hpp"
#include <cstdio>
#include <fstream>
#include <string>
#include <map>
#include <algorithm>
#include <cstdint>

using namespace mpmc_logger;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s <logfile>\n", argv[0]);
        return 1;
    }

    std::ifstream f(argv[1], std::ios::binary);
    if (!f.is_open()) {
        std::fprintf(stderr, "Error: cannot open %s\n", argv[1]);
        return 1;
    }

    // Counters
    size_t total = 0;
    size_t level_counts[7] = {};  // TRACE..OFF
    std::map<uint32_t, size_t> thread_counts;
    uint64_t min_ts = UINT64_MAX;
    uint64_t max_ts = 0;
    size_t total_payload_bytes = 0;
    uint16_t min_payload = UINT16_MAX;
    uint16_t max_payload = 0;

    LogEntry entry;
    while (f.read(reinterpret_cast<char*>(&entry), sizeof(entry))) {
        ++total;

        uint8_t lvl = static_cast<uint8_t>(entry.level);
        if (lvl <= 6) level_counts[lvl]++;

        thread_counts[entry.thread_id]++;

        if (entry.timestamp_ns < min_ts) min_ts = entry.timestamp_ns;
        if (entry.timestamp_ns > max_ts) max_ts = entry.timestamp_ns;

        total_payload_bytes += entry.payload_len;
        if (entry.payload_len < min_payload) min_payload = entry.payload_len;
        if (entry.payload_len > max_payload) max_payload = entry.payload_len;
    }

    // Report
    std::printf("\n════════════════════════════════════════════\n");
    std::printf("  MPMC Logger — Log File Statistics\n");
    std::printf("════════════════════════════════════════════\n\n");

    std::printf("  File:           %s\n", argv[1]);
    std::printf("  Total entries:  %zu\n", total);
    std::printf("  File size:      %.2f MB\n",
                static_cast<double>(total * sizeof(LogEntry)) / (1024.0 * 1024.0));

    std::printf("\n─── Level Distribution ────────────────────\n");
    const char* level_names[] = {"TRACE", "DEBUG", "INFO ", "WARN ", "ERROR", "FATAL", "OFF  "};
    for (int i = 0; i < 6; ++i) {
        if (level_counts[i] > 0) {
            double pct = 100.0 * level_counts[i] / total;
            std::printf("  [%s]  %10zu  (%5.1f%%)\n",
                        level_names[i], level_counts[i], pct);
        }
    }

    std::printf("\n─── Thread Distribution ───────────────────\n");
    for (auto& [tid, count] : thread_counts) {
        double pct = 100.0 * count / total;
        std::printf("  tid:%-8u  %10zu  (%5.1f%%)\n", tid, count, pct);
    }

    if (total > 0 && max_ts > min_ts) {
        double duration_sec = static_cast<double>(max_ts - min_ts) / 1e9;
        double entries_per_sec = total / duration_sec;

        std::printf("\n─── Throughput ────────────────────────────\n");
        std::printf("  Time span:      %.3f sec\n", duration_sec);
        std::printf("  Throughput:     %.2f M entries/sec\n",
                    entries_per_sec / 1e6);
    }

    std::printf("\n─── Payload ──────────────────────────────\n");
    std::printf("  Total payload:  %.2f MB\n",
                static_cast<double>(total_payload_bytes) / (1024.0 * 1024.0));
    if (total > 0) {
        std::printf("  Avg payload:    %.1f bytes\n",
                    static_cast<double>(total_payload_bytes) / total);
    }
    std::printf("  Min payload:    %u bytes\n", min_payload);
    std::printf("  Max payload:    %u bytes\n", max_payload);

    std::printf("\n════════════════════════════════════════════\n\n");

    return 0;
}
