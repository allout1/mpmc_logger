#pragma once

//
// Adapted from the original mpmc_2.hpp by Aashray Tandon.
// Templatized on element type T and ring buffer SIZE.
//
// Design:
//   - Bounded power-of-2 ring buffer with CAS-based sequencing
//   - Each slot is padded to DOUBLE_CACHE_LINE (128 bytes) to prevent
//     false sharing between adjacent slots
//   - Producer head and consumer tails on separate cache lines
//   - ILP-unrolled min-tail scan (4 accumulators)
//   - __builtin_prefetch for next-slot prediction
//
// Memory orders:
//   - push: relaxed load of head → acquire CAS → release store of sequence
//   - pop:  acquire load of sequence → release store of tail
//   - This ensures that data written before sequence-store is visible to
//     any thread that observes that sequence value.

#include <atomic>
#include <cstdint>
#include <cassert>
#include <stdexcept>
#include <new>
#include <algorithm>

#ifdef __AVX2__
#include <immintrin.h>
#endif

namespace mpmc_logger {

#if defined(__cpp_lib_hardware_interference_size)
constexpr size_t CACHE_LINE_SIZE = std::hardware_destructive_interference_size;
#else
constexpr size_t CACHE_LINE_SIZE = 128;
#endif
constexpr size_t DOUBLE_CACHE_LINE_SIZE = CACHE_LINE_SIZE * 2;

// Generic Fast Copy
// Compile-time dispatch: if sizeof(T) is exactly 128 and AVX2 is available,
// use 4x ymm stores; otherwise fall back to memcpy.

template <typename T>
__attribute__((always_inline))
inline void fast_copy(void* __restrict dst,
                      const void* __restrict src) noexcept {
#if defined(__AVX2__)
    if constexpr (sizeof(T) == 128) {
        __m256i y0 = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(
                static_cast<const char*>(src)));
        __m256i y1 = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(
                static_cast<const char*>(src) + 32));
        __m256i y2 = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(
                static_cast<const char*>(src) + 64));
        __m256i y3 = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(
                static_cast<const char*>(src) + 96));
        _mm256_storeu_si256(
            reinterpret_cast<__m256i*>(static_cast<char*>(dst)),      y0);
        _mm256_storeu_si256(
            reinterpret_cast<__m256i*>(static_cast<char*>(dst) + 32), y1);
        _mm256_storeu_si256(
            reinterpret_cast<__m256i*>(static_cast<char*>(dst) + 64), y2);
        _mm256_storeu_si256(
            reinterpret_cast<__m256i*>(static_cast<char*>(dst) + 96), y3);
        return;
    } else if constexpr (sizeof(T) == 96) {
        __m256i y0 = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(
                static_cast<const char*>(src)));
        __m256i y1 = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(
                static_cast<const char*>(src) + 32));
        __m256i y2 = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(
                static_cast<const char*>(src) + 64));
        _mm256_storeu_si256(
            reinterpret_cast<__m256i*>(static_cast<char*>(dst)),      y0);
        _mm256_storeu_si256(
            reinterpret_cast<__m256i*>(static_cast<char*>(dst) + 32), y1);
        _mm256_storeu_si256(
            reinterpret_cast<__m256i*>(static_cast<char*>(dst) + 64), y2);
        return;
    }
#endif
    std::memcpy(dst, src, sizeof(T));
}

// MPMC Queue implementation

template <typename T, size_t SIZE>
requires std::is_trivially_copyable_v<T> && std::is_default_constructible_v<T>
class MPMCQueue {
public:
    static_assert((SIZE & (SIZE - 1)) == 0,
                  "SIZE must be a power of 2 for bitwise index masking");
    static_assert(SIZE >= 16,
                  "SIZE must be at least 16 for meaningful buffering");

    static constexpr uint32_t MAX_CONSUMERS = 32;
    static constexpr uint64_t INVALID_POS   = UINT64_MAX;
    static constexpr uint64_t INDEX_MASK    = SIZE - 1;

    // Slot
    // Aligned to 2 cache lines.  Sequence number guards the data:
    //   sequence == expected_pos  →  data is ready to read
    //   sequence == INVALID_POS   →  slot is empty / writable
    struct alignas(DOUBLE_CACHE_LINE_SIZE) Slot {
        std::atomic<uint64_t> sequence;
        T data;
    };

    
    struct alignas(CACHE_LINE_SIZE) ConsumerInfo {
        std::atomic<uint64_t> tail{0};
        std::atomic<bool>     active{false};
    };

    
    struct alignas(DOUBLE_CACHE_LINE_SIZE) ProducerInfo {
        std::atomic<uint64_t> head{0};
    };

    
    struct alignas(DOUBLE_CACHE_LINE_SIZE) MetaInfo {
        std::atomic<uint32_t> num_consumers{0};
        std::atomic<uint64_t> min_tail{0};
    };

private:
    ConsumerInfo consumers_[MAX_CONSUMERS];
    ProducerInfo producer_;
    MetaInfo     meta_;
    Slot         slots_[SIZE];

public:
    MPMCQueue() noexcept {
        meta_.num_consumers.store(0, std::memory_order_relaxed);
        meta_.min_tail.store(0, std::memory_order_relaxed);

        for (uint32_t i = 0; i < MAX_CONSUMERS; ++i) {
            consumers_[i].tail.store(0, std::memory_order_relaxed);
            consumers_[i].active.store(false, std::memory_order_relaxed);
        }
        for (size_t i = 0; i < SIZE; ++i) {
            slots_[i].sequence.store(INVALID_POS, std::memory_order_relaxed);
        }
    }

    // Non-copyable, non-movable (contains atomics + huge array)
    MPMCQueue(const MPMCQueue&) = delete;
    MPMCQueue& operator=(const MPMCQueue&) = delete;
    MPMCQueue(MPMCQueue&&) = delete;
    MPMCQueue& operator=(MPMCQueue&&) = delete;

    
    uint32_t register_consumer() {
        uint32_t id = meta_.num_consumers.fetch_add(1, std::memory_order_relaxed);
        if (id >= MAX_CONSUMERS) {
            throw std::runtime_error("MPMCQueue: too many consumers (max 32)");
        }

        uint64_t head = producer_.head.load(std::memory_order_acquire);
        consumers_[id].tail.store(head, std::memory_order_relaxed);
        consumers_[id].active.store(true, std::memory_order_release);
        return id;
    }

    void unregister_consumer(uint32_t id) noexcept {
        consumers_[id].active.store(false, std::memory_order_release);
    }

    
    // Returns true if the entry was enqueued, false if the queue is full.
    __attribute__((hot))
    inline bool push(const T& item) noexcept {
        uint64_t head;
        for (;;) {
            head = producer_.head.load(std::memory_order_relaxed);

            // Prefetch the predicted slot into L1 for writing
            __builtin_prefetch(&slots_[head & INDEX_MASK], 1, 0);

            // Check if queue is full against cached min_tail
            uint64_t min_tail = meta_.min_tail.load(std::memory_order_acquire);
            if (head >= min_tail + SIZE) [[unlikely]] {
                uint64_t new_min = compute_min_tail();
                if (new_min != min_tail) {
                    meta_.min_tail.store(new_min, std::memory_order_release);
                    min_tail = new_min;
                }
                if (head >= min_tail + SIZE) {
                    return false;  // Queue genuinely full
                }
            }

            if (producer_.head.compare_exchange_weak(
                    head, head + 1,
                    std::memory_order_acquire,
                    std::memory_order_relaxed)) {
                break;
            }
        }

        const uint64_t index = head & INDEX_MASK;

        // Prefetch next slot for the next producer call
        __builtin_prefetch(&slots_[(head + 1) & INDEX_MASK], 1, 0);

        // Copy data into slot using AVX2 or memcpy
        fast_copy<T>(&slots_[index].data, &item);

        // Publish: make data visible to consumers
        slots_[index].sequence.store(head, std::memory_order_release);
        return true;
    }

    
    // Returns true if an entry was dequeued into `item`.
    __attribute__((hot))
    inline bool pop(uint32_t consumer_id, T& item) noexcept {
        if (!consumers_[consumer_id].active.load(std::memory_order_acquire))
            return false;

        uint64_t tail = consumers_[consumer_id].tail.load(std::memory_order_relaxed);
        uint64_t index = tail & INDEX_MASK;

        uint64_t seq = slots_[index].sequence.load(std::memory_order_acquire);
        if (seq != tail) [[unlikely]] {
            return false;  // Slot not ready yet
        }

        // Prefetch next slot for the next pop call
        __builtin_prefetch(&slots_[(tail + 1) & INDEX_MASK], 0, 1);

        // Copy data out using AVX2 or memcpy
        fast_copy<T>(&item, &slots_[index].data);

        // Advance this consumer's tail
        consumers_[consumer_id].tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    

    uint64_t head() const noexcept {
        return producer_.head.load(std::memory_order_relaxed);
    }

    uint64_t tail(uint32_t id) const noexcept {
        return consumers_[id].tail.load(std::memory_order_relaxed);
    }

    uint32_t num_consumers() const noexcept {
        return meta_.num_consumers.load(std::memory_order_relaxed);
    }

    size_t capacity() const noexcept {
        return SIZE;
    }

    
    // Uses 4 independent accumulators to break sequential dependency chain
    // and exploit instruction-level parallelism on OoO cores.

    uint64_t compute_min_tail() const noexcept {
        uint64_t min0 = INVALID_POS, min1 = INVALID_POS,
                 min2 = INVALID_POS, min3 = INVALID_POS;

        for (uint32_t i = 0; i < MAX_CONSUMERS; i += 4) {
            if (consumers_[i].active.load(std::memory_order_acquire))
                min0 = std::min(min0,
                    consumers_[i].tail.load(std::memory_order_acquire));

            if (consumers_[i + 1].active.load(std::memory_order_acquire))
                min1 = std::min(min1,
                    consumers_[i + 1].tail.load(std::memory_order_acquire));

            if (consumers_[i + 2].active.load(std::memory_order_acquire))
                min2 = std::min(min2,
                    consumers_[i + 2].tail.load(std::memory_order_acquire));

            if (consumers_[i + 3].active.load(std::memory_order_acquire))
                min3 = std::min(min3,
                    consumers_[i + 3].tail.load(std::memory_order_acquire));
        }

        uint64_t result = std::min(std::min(min0, min1), std::min(min2, min3));
        return (result == INVALID_POS) ? 0 : result;
    }

    void recompute_and_store_min_tail() noexcept {
        meta_.min_tail.store(compute_min_tail(), std::memory_order_release);
    }
};

} // namespace mpmc_logger
