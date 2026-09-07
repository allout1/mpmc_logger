#pragma once

//
// Double-buffered mmap writer.  While one region accepts writes (memcpy),
// the other is being flushed to disk (msync).  This eliminates write()
// syscall overhead entirely — we just memcpy into mapped pages.
//
// Platform notes:
//   - Linux: uses MAP_POPULATE to pre-fault pages, mremap if available
//   - macOS: falls back to MAP_SHARED without MAP_POPULATE, no mremap
//
// File rotation:
//   When total bytes written to the current file exceed max_file_size,
//   we close the current file, rename it (app.log → app.log.1), and
//   open a fresh file.

#include <cstddef>
#include <cstdint>
#include <string>
#include <atomic>
#include <span>

namespace mpmc_logger {

class MMapSink {
public:
    
    //
    // path:            Base path for log files (e.g., "logs/app.log")
    // region_size:     Size of each mmap region (default 64 MB)
    // max_file_size:   Max bytes per file before rotation (default 1 GB)
    // max_rotated:     Number of old files to keep (default 8)
    MMapSink(const std::string& path,
             size_t region_size   = 64 * 1024 * 1024,
             size_t max_file_size = 1024 * 1024 * 1024,
             size_t max_rotated   = 8);

    ~MMapSink();

    // Non-copyable, non-movable
    MMapSink(const MMapSink&) = delete;
    MMapSink& operator=(const MMapSink&) = delete;
    MMapSink(MMapSink&&) = delete;
    MMapSink& operator=(MMapSink&&) = delete;

    
    // Copies bytes from `data` into the current mmap region.
    // If the region is full, triggers a rotation.
    // Returns number of bytes written (always `data.size()` on success, 0 on failure).
    size_t write(std::span<const char> data) noexcept;

    
    // Synchronous flush of all pending data to disk.
    // Call on shutdown or when you need durability guarantees.
    void flush() noexcept;

    
    uint64_t total_bytes_written() const noexcept { return total_bytes_written_; }
    uint64_t total_rotations() const noexcept { return rotation_count_; }

private:
    
    void open_file();
    void close_file() noexcept;
    void map_region();
    void unmap_region() noexcept;
    void rotate_files();
    void ensure_directory(const std::string& path);

    std::string base_path_;
    size_t      region_size_;
    size_t      max_file_size_;
    size_t      max_rotated_;

    int         fd_            = -1;
    char*       mmap_ptr_      = nullptr;
    size_t      mmap_offset_   = 0;       // Current write offset within region
    size_t      file_offset_   = 0;       // Total bytes written to current file

    uint64_t    total_bytes_written_ = 0;
    uint64_t    rotation_count_      = 0;
};

} // namespace mpmc_logger
