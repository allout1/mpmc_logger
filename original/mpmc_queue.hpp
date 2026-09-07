#pragma once

#include <atomic>

#include <cstdint>

#include <cstring>

#include <immintrin.h>

#include <array>

#include <stdexcept>

#include <sys/mman.h>

#include <sys/stat.h>

#include <fcntl.h>

#include <unistd.h>

#include <iostream>

#include <unordered_map>

#include <chrono>

constexpr size_t CACHE_LINE = 64;

// data to be pushed/popped into the queue

// #pragma pack(push, 1)

struct alignas(CACHE_LINE) MarketData {

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

template <size_t SIZE>

class MPMC_Queue {

public:
  static_assert((SIZE & (SIZE - 1)) == 0,
                "Size be Power of 2 for ease and lower latency");

  static constexpr uint32_t MAX_CONSUMERS = 32;

  static constexpr uint32_t MAX_TOKENS = (1 << 13);

  static constexpr uint64_t invalid_pos = UINT64_MAX;

  struct alignas(CACHE_LINE) Slot {

    std::atomic<uint64_t> sequence;

    MarketData data;
  };

  struct alignas(CACHE_LINE) ConsumerInfo {

    std::atomic<uint64_t> tail{0};

    std::atomic<bool> active{false};

    char _pad[CACHE_LINE - sizeof(std::atomic<uint64_t>) -
              sizeof(std::atomic<bool>)];

  } consumers_[MAX_CONSUMERS];

  struct alignas(CACHE_LINE) {

    std::atomic<uint64_t> head{0};

    char _pad[CACHE_LINE - sizeof(std::atomic<uint64_t>)];

  } producer;

  struct alignas(CACHE_LINE) {

    std::atomic<uint32_t> num_consumers{0};

    std::atomic<uint64_t> min_tail{0}; // cached minimum of all tails

    char _pad[CACHE_LINE - sizeof(std::atomic<uint32_t>) -
              3 * sizeof(std::atomic<uint64_t>)];

  } meta_;

  Slot slots[SIZE];

  MPMC_Queue() {

    meta_.num_consumers.store(0, std::memory_order_relaxed);

    meta_.min_tail.store(0, std::memory_order_relaxed);

    for (int i = 0; i < MAX_CONSUMERS; i++) {

      consumers_[i].tail.store(0, std::memory_order_relaxed);

      consumers_[i].active.store(false, std::memory_order_relaxed);
    }

    for (int i = 0; i < SIZE; i++) {

      slots[i].sequence.store(invalid_pos, std::memory_order_relaxed);
    }

    purge_lock.store(false, std::memory_order_relaxed);
  }

  uint32_t reg_consumer() {

    uint32_t id = meta_.num_consumers.fetch_add(1, std::memory_order_relaxed);

    if (id >= MAX_CONSUMERS)

      throw std::runtime_error("Too many consumers");

    uint64_t head_ = producer.head.load(std::memory_order_acquire);

    consumers_[id].tail.store(head_, std::memory_order_relaxed);

    consumers_[id].active.store(true, std::memory_order_release);

    uint64_t tail_ = consumers_[id].tail.load(std::memory_order_acquire);

    // std::cout<<"INIT:head_:"<<head_<<" , tail:"<< tail_<<std::endl;

    return id;
  }

  __attribute__((hot))

  inline bool
  push(MarketData &data) {

    uint64_t head;

    for (int spin = 0;; spin++) {

      head = producer.head.load(std::memory_order_relaxed);

      uint64_t min_tail = meta_.min_tail.load(std::memory_order_acquire);

      if ((head >= min_tail + SIZE)) [[unlikely]] {

        uint64_t new_min = computeMinTail();

        if (new_min != min_tail) {

          meta_.min_tail.store(new_min, std::memory_order_release);

          min_tail = new_min;
        }

        if (head >= min_tail + SIZE) {

          return false;

          continue;
        }
      }

      // else break;

      if (producer.head.compare_exchange_weak(head, head + 1,
                                              std::memory_order_acquire,
                                              std::memory_order_relaxed)) {

        break;
      }
    }

    uint64_t index = head & (SIZE - 1);

    std::memcpy(&slots[index].data, &data, sizeof(MarketData));

    // std::cout<<"Index: "<<index<<" Head: "<<head<<std::endl;

    slots[index].sequence.store(head, std::memory_order_release);

    return true;
  }

  __attribute__((hot))

  inline bool
  pop(uint32_t consumer_id, MarketData &item) {

    if (!consumers_[consumer_id].active.load(std::memory_order_acquire))
      return false;

    uint64_t tail =
        consumers_[consumer_id].tail.load(std::memory_order_relaxed);

    uint64_t index = tail & (SIZE - 1);

    uint64_t seq = slots[index].sequence.load(std::memory_order_acquire);

    if (seq != tail) [[unlikely]] {

      // std::cout<<"NOT-PoP:Seq:"
      // <<seq<<",index:"<<index<<",tail:"<<tail<<std::endl;

      return false;
    }

    // std::cout<<"POP:Seq:" <<seq<<",index:"<<index<<",tail:"<<tail<<std::endl;

    std::memcpy(&item, &slots[index].data, sizeof(MarketData));

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

  uint64_t computeMinTail() {

    // uint32_t num = meta_.num_consumers.load(std::memory_order_acquire);

    uint64_t mini = invalid_pos;

    for (uint32_t i = 0; i < MAX_CONSUMERS; i++) {

      if (!consumers_[i].active.load(std::memory_order_acquire))
        continue;

      uint64_t t = consumers_[i].tail.load(std::memory_order_acquire);

      mini = std::min(mini, t);
    }

    mini = (mini == invalid_pos) ? 0 : mini;

    meta_.min_tail.store(mini, std::memory_order_release);

    return mini;
  }

  void recomputeAndStoreMinTail() {

    meta_.min_tail.store(computeMinTail(), std::memory_order_release);
  }
};

template <typename QueueType>

class ShmQueueManager {

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

    }

    else {

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

    queue_ = static_cast<QueueType *>(mmap(

        nullptr, sizeof(QueueType),

        PROT_READ | PROT_WRITE,

        MAP_SHARED | MAP_POPULATE,

        fd_, 0));

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

// #pragma pack(pop)