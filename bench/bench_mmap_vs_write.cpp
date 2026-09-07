
//
// Compares sequential write throughput:
//   1. mmap + memcpy (our approach)
//   2. write() syscall
//   3. fwrite() buffered I/O

#include "mpmc_logger/timestamp.hpp"
#include <benchmark/benchmark.h>
#include <cstring>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <filesystem>

namespace fs = std::filesystem;

#ifndef MAP_POPULATE
#define MAP_POPULATE 0
#endif

static const std::string BENCH_DIR = "/tmp/mpmc_logger_bench_io";
static constexpr size_t BLOCK_SIZE = 128;  // Same as LogEntry
static constexpr size_t TOTAL_WRITES = 1000000;  // 1M writes = 128 MB



static void BM_Mmap_Memcpy(benchmark::State& state) {
    fs::remove_all(BENCH_DIR);
    fs::create_directories(BENCH_DIR);
    std::string path = BENCH_DIR + "/mmap_bench.dat";

    char block[BLOCK_SIZE];
    std::memset(block, 'A', BLOCK_SIZE);

    for (auto _ : state) {
        int fd = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
        size_t total_size = BLOCK_SIZE * TOTAL_WRITES;
        ftruncate(fd, total_size);

        char* ptr = static_cast<char*>(mmap(
            nullptr, total_size, PROT_READ | PROT_WRITE,
            MAP_SHARED | MAP_POPULATE, fd, 0));

        for (size_t i = 0; i < TOTAL_WRITES; ++i) {
            std::memcpy(ptr + i * BLOCK_SIZE, block, BLOCK_SIZE);
        }

        msync(ptr, total_size, MS_ASYNC);
        munmap(ptr, total_size);
        close(fd);
    }

    state.SetBytesProcessed(
        static_cast<int64_t>(state.iterations()) * BLOCK_SIZE * TOTAL_WRITES);
    state.SetItemsProcessed(
        static_cast<int64_t>(state.iterations()) * TOTAL_WRITES);

    fs::remove_all(BENCH_DIR);
}
BENCHMARK(BM_Mmap_Memcpy)->Iterations(3)->UseRealTime();



static void BM_Write_Syscall(benchmark::State& state) {
    fs::remove_all(BENCH_DIR);
    fs::create_directories(BENCH_DIR);
    std::string path = BENCH_DIR + "/write_bench.dat";

    char block[BLOCK_SIZE];
    std::memset(block, 'B', BLOCK_SIZE);

    for (auto _ : state) {
        int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);

        for (size_t i = 0; i < TOTAL_WRITES; ++i) {
            write(fd, block, BLOCK_SIZE);
        }

        fsync(fd);
        close(fd);
    }

    state.SetBytesProcessed(
        static_cast<int64_t>(state.iterations()) * BLOCK_SIZE * TOTAL_WRITES);
    state.SetItemsProcessed(
        static_cast<int64_t>(state.iterations()) * TOTAL_WRITES);

    fs::remove_all(BENCH_DIR);
}
BENCHMARK(BM_Write_Syscall)->Iterations(3)->UseRealTime();



static void BM_Fwrite_Buffered(benchmark::State& state) {
    fs::remove_all(BENCH_DIR);
    fs::create_directories(BENCH_DIR);
    std::string path = BENCH_DIR + "/fwrite_bench.dat";

    char block[BLOCK_SIZE];
    std::memset(block, 'C', BLOCK_SIZE);

    for (auto _ : state) {
        FILE* fp = fopen(path.c_str(), "wb");

        for (size_t i = 0; i < TOTAL_WRITES; ++i) {
            fwrite(block, 1, BLOCK_SIZE, fp);
        }

        fflush(fp);
        fclose(fp);
    }

    state.SetBytesProcessed(
        static_cast<int64_t>(state.iterations()) * BLOCK_SIZE * TOTAL_WRITES);
    state.SetItemsProcessed(
        static_cast<int64_t>(state.iterations()) * TOTAL_WRITES);

    fs::remove_all(BENCH_DIR);
}
BENCHMARK(BM_Fwrite_Buffered)->Iterations(3)->UseRealTime();

BENCHMARK_MAIN();
