#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <immintrin.h>
#include <iostream>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>

constexpr size_t CACHE_LINE = 64;
// Used for highly contended atomics to prevent hardware spatial prefetchers
// from crossing cache line boundaries (Intel prefetchers often fetch pairs).
constexpr size_t DOUBLE_CACHE_LINE = 128;

// Removed alignas(CACHE_LINE) from the struct itself to pack it tighter inside
// Slot. The structure naturally aligns to 96 bytes.
struct MarketData {
  uint32_t token;
  std::array<uint32_t, 5> bids{};
  std::array<uint32_t, 5> asks{};
  std::array<uint32_t, 5> bids_quantity{};
  std::array<uint32_t, 5> asks_quantity{};
  uint32_t seq_no = 0;
  uint64_t push_ts_n = 0;

  MarketData()
      : token(0), bids(), asks(), bids_quantity(), asks_quantity(), seq_no(0),
        push_ts_n(0) {}
};

// Guarantee it's exactly 96 bytes so our AVX copy doesn't cause UB
static_assert(sizeof(MarketData) == 96,
              "MarketData size must be exactly 96 bytes for AVX2 copy");

// AVX2 routine to bypass memcpy overhead and exploit ILP
__attribute__((always_inline)) inline void
fast_copy_96(void *__restrict dst, const void *__restrict src) {
#if defined(__AVX2__)
  __m256i y0 = _mm256_loadu_si256((const __m256i *)src);
  __m256i y1 = _mm256_loadu_si256((const __m256i *)((const char *)src + 32));
  __m256i y2 = _mm256_loadu_si256((const __m256i *)((const char *)src + 64));
  _mm256_storeu_si256((__m256i *)dst, y0);
  _mm256_storeu_si256((__m256i *)((char *)dst + 32), y1);
  _mm256_storeu_si256((__m256i *)((char *)dst + 64), y2);
#else
  std::memcpy(dst, src, 96);
#endif
}

template <typename T, size_t SIZE> class MPMC_Queue {
public:
  static_assert((SIZE & (SIZE - 1)) == 0,
                "Size be Power of 2 for ease and lower latency");
  static constexpr uint32_t MAX_CONSUMERS = 32;
  static constexpr uint32_t MAX_TOKENS = (1 << 13);
  static constexpr uint64_t invalid_pos = UINT64_MAX;

  // Slot is aligned to 128 bytes (2 cache lines).
  // Sequence (8) + Data (96) = 104 bytes. Prevents false sharing between
  // adjacent slots.
  struct alignas(DOUBLE_CACHE_LINE) Slot {
    std::atomic<uint64_t> sequence;
    T data;
  };

  // Strict cache line alignment handles padding natively based on ABI
  struct alignas(CACHE_LINE) ConsumerInfo {
    std::atomic<uint64_t> tail{0};
    std::atomic<bool> active{false};
  };

  // Isolate highly contended Producer head
  struct alignas(DOUBLE_CACHE_LINE) ProducerInfo {
    std::atomic<uint64_t> head{0};
  };

  // Isolate metadata from both producer head and consumer array
  struct alignas(DOUBLE_CACHE_LINE) MetaInfo {
    std::atomic<uint32_t> num_consumers{0};
    std::atomic<uint64_t> min_tail{0};
    std::atomic<bool> purge_lock{
        false}; // Added to fix compilation from constructor
  };

  ConsumerInfo consumers_[MAX_CONSUMERS];
  ProducerInfo producer;
  MetaInfo meta_;
  Slot slots[SIZE];

  MPMC_Queue() {
    meta_.num_consumers.store(0, std::memory_order_relaxed);
    meta_.min_tail.store(0, std::memory_order_relaxed);
    meta_.purge_lock.store(false, std::memory_order_relaxed);

    for (int i = 0; i < MAX_CONSUMERS; i++) {
      consumers_[i].tail.store(0, std::memory_order_relaxed);
      consumers_[i].active.store(false, std::memory_order_relaxed);
    }
    for (int i = 0; i < SIZE; i++) {
      slots[i].sequence.store(invalid_pos, std::memory_order_relaxed);
    }
  }

  uint32_t reg_consumer() {
    uint32_t id = meta_.num_consumers.fetch_add(1, std::order_relaxed);
    if (id >= MAX_CONSUMERS)
      throw std::runtime_error("Too many consumers");

    uint64_t head_ = producer.head.load(std::memory_order_acquire);
    consumers_[id].tail.store(head_, std::memory_order_relaxed);
    consumers_[id].active.store(true, std::memory_order_release);
    return id;
  }

  __attribute__((hot)) inline bool push(MarketData &data) {
    uint64_t head;
    for (;;) {
      head = producer.head.load(std::memory_order_relaxed);

      // Prefetch the predicted slot memory into L1 *before* doing CAS loop work
      // 1 = prepare for write, 0 = non-temporal (we write once and walk away)
      __builtin_prefetch(&slots[(head) & (SIZE - 1)], 1, 0);

      uint64_t min_tail = meta_.min_tail.load(std::memory_order_acquire);

      if (head >= min_tail + SIZE) __attribute__((unlikely)) {
        uint64_t new_min = computeMinTail();
        if (new_min != min_tail) {
          meta_.min_tail.store(new_min, std::memory_order_release);
          min_tail = new_min;
        }
        if (head >= min_tail + SIZE) {
          return false; // Queue full
        }
      }

      if (producer.head.compare_exchange_weak(head, head + 1,
                                              std::memory_order_acquire,
                                              std::memory_order_relaxed)) {
        break;
      }
    }

    uint64_t index = head & (SIZE - 1);

    // Prefetch the NEXT slot ahead of time for the next caller
    __builtin_prefetch(&slots[(head + 1) & (SIZE - 1)], 1, 0);

    // AVX ILP copy replaces std::memcpy
    fast_copy_96(&slots[index].data, &data);

    slots[index].sequence.store(head, std::memory_order_release);
    return true;
  }

  __attribute__((hot)) inline bool pop(uint32_t consumer_id, MarketData &item) {
    if (!consumers_[consumer_id].active.load(std::memory_order_acquire))
      return false;

    uint64_t tail =
        consumers_[consumer_id].tail.load(std::memory_order_relaxed);
    uint64_t index = tail & (SIZE - 1);

    uint64_t seq = slots[index].sequence.load(std::memory_order_acquire);
    if (seq != tail) __attribute__((unlikely)) {
      return false;
    }

    // Prefetch the NEXT slot we will want to read
    __builtin_prefetch(&slots[(tail + 1) & (SIZE - 1)], 0, 1);

    // AVX ILP copy replaces std::memcpy
    fast_copy_96(&item, &slots[index].data);

    consumers_[consumer_id].tail.store(tail + 1, std::memory_order_release);
    return true;
  }

  void del_consumer(uint32_t consumer_id) {
    consumers_[consumer_id].active.store(false, std::memory_order_release);
  }

  uint64_t head() { return producer.head.load(std::memory_order_relaxed); }

  uint64_t tail(uint32_t id) {
    return consumers_[id].tail.load(std::memory_order_relaxed);
  }

  // Unrolled to 4 accumulator chains to break sequential loop dependency and
  // enforce ILP
  uint64_t computeMinTail() {
    uint64_t min0 = invalid_pos, min1 = invalid_pos, min2 = invalid_pos,
             min3 = invalid_pos;

    for (uint32_t i = 0; i < MAX_CONSUMERS; i += 4) {
      if (consumers_[i].active.load(std::memory_order_acquire))
        min0 =
            std::min(min0, consumers_[i].tail.load(std::memory_order_acquire));

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

    uint64_t mini = std::min(std::min(min0, min1), std::min(min2, min3));

    mini = (mini == invalid_pos) ? 0 : mini;
    meta_.min_tail.store(mini, std::memory_order_release);
    return mini;
  }

  void recomputeAndStoreMinTail() {
    meta_.min_tail.store(computeMinTail(), std::memory_order_release);
  }
};

template <typename QueueType> class ShmQueueManager {
  const char *shm_name_;
  int fd_ = -1;
  QueueType *queue_ = nullptr;
  bool is_creator_;

public:
  ShmQueueManager(const char *name, bool create)
      : shm_name_(name), is_creator_(create) {

    if (create) {
      shm_unlink(shm_name_);
      fd_ = shm_open(shm_name_, O_CREAT | O_RDWR, 0666);
      if (fd_ < 0) {
        perror("shm_open (create)");
        throw std::runtime_error("shm_open failed");
      }
      if (ftruncate(fd_, sizeof(QueueType)) < 0) {
        perror("ftruncate");
        close(fd_);
        throw std::runtime_error("ftruncate failed");
      }
    } else {
      for (int i = 0; i < 50; ++i) {
        fd_ = shm_open(shm_name_, O_RDWR, 0666);
        if (fd_ >= 0)
          break;
        usleep(10'000);
      }
      if (fd_ < 0) {
        perror("shm_open (attach)");
        throw std::runtime_error("shm_open failed");
      }
    }

    queue_ = static_cast<QueueType *>(mmap(nullptr, sizeof(QueueType),
                                           PROT_READ | PROT_WRITE,
                                           MAP_SHARED | MAP_POPULATE, fd_, 0));

    if (queue_ == MAP_FAILED) {
      perror("mmap");
      close(fd_);
      throw std::runtime_error("mmap failed");
    }

    if (mlock(queue_, sizeof(QueueType)) < 0)
      perror("mlock (warning, continuing)");

    if (create)
      new (queue_) QueueType();
  }

  ~ShmQueueManager() {
    if (queue_) {
      munlock(queue_, sizeof(QueueType));
      munmap(queue_, sizeof(QueueType));
    }
    if (fd_ >= 0)
      close(fd_);
    if (is_creator_)
      shm_unlink(shm_name_);
  }

  QueueType *get() { return queue_; }
};