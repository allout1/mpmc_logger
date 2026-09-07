#include "mpmc_logger/mmap_sink.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

// MAP_POPULATE is Linux-only; define as 0 on macOS so the flag is harmless
#ifndef MAP_POPULATE
#define MAP_POPULATE 0
#endif

namespace mpmc_logger {



MMapSink::MMapSink(const std::string& path,
                   size_t region_size,
                   size_t max_file_size,
                   size_t max_rotated)
    : base_path_(path)
    , region_size_(region_size)
    , max_file_size_(max_file_size)
    , max_rotated_(max_rotated)
{
    // Ensure the parent directory exists
    auto slash = base_path_.rfind('/');
    if (slash != std::string::npos) {
        ensure_directory(base_path_.substr(0, slash));
    }

    open_file();
    map_region();
}



MMapSink::~MMapSink() {
    flush();
    unmap_region();
    close_file();
}



size_t MMapSink::write(std::span<const char> data) noexcept {
    size_t len = data.size();
    if (len == 0) return 0;

    if (!mmap_ptr_) return 0;

    // If this write would exceed the region, rotate
    if (mmap_offset_ + len > region_size_) {
        // Sync what we have
        if (mmap_ptr_ && mmap_offset_ > 0) {
            ::msync(mmap_ptr_, mmap_offset_, MS_ASYNC);
        }
        unmap_region();

        // Check if we need to rotate the file
        if (file_offset_ + len > max_file_size_) {
            close_file();
            rotate_files();
            open_file();
        }

        map_region();
        if (!mmap_ptr_) return 0;
    }

    // memcpy into the mapped region — this is the fast path
    std::memcpy(mmap_ptr_ + mmap_offset_, data.data(), len);
    mmap_offset_        += len;
    file_offset_        += len;
    total_bytes_written_ += len;

    return len;
}



void MMapSink::flush() noexcept {
    if (mmap_ptr_ && mmap_offset_ > 0) {
        ::msync(mmap_ptr_, mmap_offset_, MS_SYNC);
    }
}



void MMapSink::open_file() {
    fd_ = ::open(base_path_.c_str(),
                 O_RDWR | O_CREAT | O_APPEND,
                 0644);
    if (fd_ < 0) {
        throw std::runtime_error(
            std::string("MMapSink: failed to open ") + base_path_
            + ": " + std::strerror(errno));
    }

    // Get current file size (in case of append)
    struct stat st;
    if (::fstat(fd_, &st) == 0) {
        file_offset_ = static_cast<size_t>(st.st_size);
    }
}



void MMapSink::close_file() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    file_offset_ = 0;
}



void MMapSink::map_region() {
    if (fd_ < 0) return;

    // Extend file to cover this new region
    size_t new_file_end = file_offset_ + region_size_;
    if (::ftruncate(fd_, static_cast<off_t>(new_file_end)) < 0) {
        std::perror("MMapSink: ftruncate");
        return;
    }

    mmap_ptr_ = static_cast<char*>(::mmap(
        nullptr,
        region_size_,
        PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_POPULATE,
        fd_,
        static_cast<off_t>(file_offset_)
    ));

    if (mmap_ptr_ == MAP_FAILED) {
        std::perror("MMapSink: mmap");
        mmap_ptr_ = nullptr;
        return;
    }

    // Hint to kernel: sequential write pattern
#ifdef MADV_SEQUENTIAL
    ::madvise(mmap_ptr_, region_size_, MADV_SEQUENTIAL);
#endif

    mmap_offset_ = 0;
}



void MMapSink::unmap_region() noexcept {
    if (mmap_ptr_) {
        ::munmap(mmap_ptr_, region_size_);
        mmap_ptr_ = nullptr;
    }
    mmap_offset_ = 0;
}


//
// Rotation scheme:
//   app.log.3 → deleted (if max_rotated_ == 3)
//   app.log.2 → app.log.3
//   app.log.1 → app.log.2
//   app.log   → app.log.1

void MMapSink::rotate_files() {
    rotation_count_++;

    // Delete the oldest file
    std::string oldest = base_path_ + "." + std::to_string(max_rotated_);
    ::unlink(oldest.c_str());

    // Shift existing rotated files
    for (size_t i = max_rotated_ - 1; i >= 1; --i) {
        std::string from = base_path_ + "." + std::to_string(i);
        std::string to   = base_path_ + "." + std::to_string(i + 1);
        ::rename(from.c_str(), to.c_str());
    }

    // Rotate current file
    std::string rotated = base_path_ + ".1";
    ::rename(base_path_.c_str(), rotated.c_str());
}



void MMapSink::ensure_directory(const std::string& path) {
    // Simple recursive mkdir
    std::string accumulated;
    for (size_t i = 0; i < path.size(); ++i) {
        accumulated += path[i];
        if (path[i] == '/' || i == path.size() - 1) {
            ::mkdir(accumulated.c_str(), 0755);
        }
    }
}

} // namespace mpmc_logger
