#include "mpmc_logger/logger.hpp"

#include <iostream>
#include <cstring>

#ifdef __x86_64__
#include <immintrin.h>
#endif

namespace mpmc_logger {



void Logger::init(const LoggerConfig& config) {
    if (initialized_.load(std::memory_order_acquire)) {
        return;  // Already initialized
    }

    // Store config
    min_level_.store(config.min_level, std::memory_order_relaxed);
    output_format_          = config.output_format;
    drain_batch_size_       = config.drain_batch_size;
    spin_count_before_yield_ = config.spin_count_before_yield;

    // Create the queue
    queue_ = std::make_unique<QueueType>();

    // Register the drain thread as a consumer
    consumer_id_ = queue_->register_consumer();

    // Create the sink
    sink_ = std::make_unique<MMapSink>(
        config.log_file_path,
        config.mmap_region_size,
        config.max_file_size,
        config.max_rotated_files
    );

    // Create text formatter if needed
    if (output_format_ == OutputFormat::TEXT) {
        text_fmt_ = std::make_unique<TextFormatter>();
    }

    // Start drain thread
    running_.store(true, std::memory_order_release);
    drain_thread_ = std::jthread([this](std::stop_token st) { drain_loop(std::move(st)); });

    initialized_.store(true, std::memory_order_release);
}



void Logger::shutdown() noexcept {
    if (!initialized_.exchange(false, std::memory_order_acq_rel)) {
        return;  // Not initialized or already shut down
    }

    // Signal drain thread to stop
    running_.store(false, std::memory_order_release);
    drain_thread_.request_stop();

    // Join drain thread
    if (drain_thread_.joinable()) {
        drain_thread_.join();
    }

    // Flush the sink
    if (sink_) {
        sink_->flush();
    }

    // Cleanup
    if (queue_) {
        queue_->unregister_consumer(consumer_id_);
    }
    queue_.reset();
    sink_.reset();
    text_fmt_.reset();
}


//
// Tight loop:
//   1. Try to pop up to drain_batch_size_ entries
//   2. Format each and write to sink
//   3. If no entries popped, spin with _mm_pause() then yield

void Logger::drain_loop(std::stop_token st) {
    LogEntry entry;
    char format_buf[TextFormatter::max_formatted_size()];
    size_t consecutive_empty = 0;

    while (!st.stop_requested() || true) {
        bool got_any = false;

        for (size_t batch = 0; batch < drain_batch_size_; ++batch) {
            if (!queue_->pop(consumer_id_, entry)) {
                break;
            }

            got_any = true;
            entries_drained_.fetch_add(1, std::memory_order_relaxed);

            // Format and write
            size_t written = 0;
            if (output_format_ == OutputFormat::BINARY) {
                written = BinaryFormatter::format(entry, format_buf,
                                                   sizeof(format_buf));
            } else {
                written = text_fmt_->format(entry, format_buf,
                                             sizeof(format_buf));
            }

            if (written > 0 && sink_) {
                sink_->write(std::span<const char>(format_buf, written));
            }
        }

        if (got_any) {
            consecutive_empty = 0;
        } else {
            // Check if we should exit (queue empty AND stop requested)
            if (st.stop_requested()) {
                // Final drain — try one more time to catch stragglers
                while (queue_->pop(consumer_id_, entry)) {
                    entries_drained_.fetch_add(1, std::memory_order_relaxed);

                    size_t written = 0;
                    if (output_format_ == OutputFormat::BINARY) {
                        written = BinaryFormatter::format(
                            entry, format_buf, sizeof(format_buf));
                    } else {
                        written = text_fmt_->format(
                            entry, format_buf, sizeof(format_buf));
                    }
                    if (written > 0 && sink_) {
                        sink_->write(std::span<const char>(format_buf, written));
                    }
                }
                break;  // Exit drain loop
            }

            // Adaptive backoff
            ++consecutive_empty;

            if (consecutive_empty < spin_count_before_yield_) {
                // CPU-friendly spin — hints the core that we're in a spin-wait
#ifdef __x86_64__
                _mm_pause();
#elif defined(__aarch64__)
                __asm__ volatile("yield" ::: "memory");
#endif
            } else {
                // After enough spins, yield the CPU
                std::this_thread::yield();
                consecutive_empty = spin_count_before_yield_;  // Don't overflow
            }
        }
    }
}

} // namespace mpmc_logger
