// ─── Binary Log Reader ──────────────────────────────────────────────────────
//
// Reads binary log files (sequence of 128-byte LogEntry structs) and
// prints them in human-readable format.
//
// Usage:
//   ./log_reader <logfile> [--level WARN] [--thread 1234] [--limit 100]

#include "mpmc_logger/log_entry.hpp"
#include "mpmc_logger/formatter.hpp"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <cstdlib>

using namespace mpmc_logger;

struct ReaderOptions {
    std::string filename;
    LogLevel    min_level = LogLevel::LVL_TRACE;
    uint32_t    filter_tid = 0;
    bool        has_tid_filter = false;
    size_t      limit = 0;  // 0 = unlimited
};

static LogLevel parse_level(const char* s) {
    if (std::strcmp(s, "TRACE") == 0) return LogLevel::LVL_TRACE;
    if (std::strcmp(s, "DEBUG") == 0) return LogLevel::LVL_DEBUG;
    if (std::strcmp(s, "INFO")  == 0) return LogLevel::LVL_INFO;
    if (std::strcmp(s, "WARN")  == 0) return LogLevel::LVL_WARN;
    if (std::strcmp(s, "ERROR") == 0) return LogLevel::LVL_ERROR;
    if (std::strcmp(s, "FATAL") == 0) return LogLevel::LVL_FATAL;
    std::fprintf(stderr, "Unknown level: %s\n", s);
    return LogLevel::LVL_TRACE;
}

static ReaderOptions parse_args(int argc, char** argv) {
    ReaderOptions opts;

    if (argc < 2) {
        std::fprintf(stderr,
            "Usage: %s <logfile> [--level LEVEL] [--thread TID] [--limit N]\n",
            argv[0]);
        std::exit(1);
    }

    opts.filename = argv[1];

    for (int i = 2; i < argc; i += 2) {
        if (i + 1 >= argc) break;
        if (std::strcmp(argv[i], "--level") == 0) {
            opts.min_level = parse_level(argv[i + 1]);
        } else if (std::strcmp(argv[i], "--thread") == 0) {
            opts.filter_tid = static_cast<uint32_t>(std::atoi(argv[i + 1]));
            opts.has_tid_filter = true;
        } else if (std::strcmp(argv[i], "--limit") == 0) {
            opts.limit = static_cast<size_t>(std::atoi(argv[i + 1]));
        }
    }

    return opts;
}

int main(int argc, char** argv) {
    auto opts = parse_args(argc, argv);

    std::ifstream f(opts.filename, std::ios::binary);
    if (!f.is_open()) {
        std::fprintf(stderr, "Error: cannot open %s\n", opts.filename.c_str());
        return 1;
    }

    TextFormatter fmt;
    char out_buf[TextFormatter::max_formatted_size()];

    LogEntry entry;
    size_t count = 0;
    size_t total = 0;

    while (f.read(reinterpret_cast<char*>(&entry), sizeof(entry))) {
        ++total;

        // Apply filters
        if (static_cast<uint8_t>(entry.level) <
            static_cast<uint8_t>(opts.min_level))
            continue;

        if (opts.has_tid_filter && entry.thread_id != opts.filter_tid)
            continue;

        // Format and print
        size_t n = fmt.format(entry, out_buf, sizeof(out_buf));
        if (n > 0) {
            fwrite(out_buf, 1, n, stdout);
        }

        ++count;
        if (opts.limit > 0 && count >= opts.limit) break;
    }

    std::fprintf(stderr, "\n--- %zu entries displayed out of %zu total ---\n",
                 count, total);
    return 0;
}
