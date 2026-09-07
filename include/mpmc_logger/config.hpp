#pragma once

#include "log_entry.hpp"
#include <cstddef>
#include <string>

namespace mpmc_logger {



enum class OutputFormat : uint8_t {
    BINARY,     // Raw LogEntry bytes — maximum throughput, decoded offline
    TEXT        // Human-readable formatted text — development / debugging
};



struct LoggerConfig {
    
    size_t queue_capacity   = 1 << 20;  // 1M slots (~128 MB at 128 bytes/entry)

    
    std::string log_file_path   = "logs/app.log";
    size_t mmap_region_size     = 64 * 1024 * 1024;   // 64 MB per mmap region
    size_t max_file_size        = 1024 * 1024 * 1024;  // 1 GB per log file
    size_t max_rotated_files    = 8;                    // Keep last N rotated files

    
    OutputFormat output_format  = OutputFormat::BINARY;

    
    LogLevel min_level          = LogLevel::LVL_TRACE;      // Log everything by default

    
    size_t drain_batch_size     = 256;   // Max entries to drain per loop iteration
    size_t spin_count_before_yield = 1000;  // _mm_pause() spins before sched_yield()
};

} // namespace mpmc_logger
