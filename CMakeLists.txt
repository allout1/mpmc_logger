cmake_minimum_required(VERSION 3.16)
project(mpmc_logger
    LANGUAGES CXX
    VERSION 1.0.0
    DESCRIPTION "Ultra-low-latency HFT logging framework with lock-free MPMC queue and mmap I/O"
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Compiler Flags
# Release: full optimization for benchmarks
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

# Try to enable march=native and mavx2, but don't fail on ARM
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-march=native" HAS_MARCH_NATIVE)
check_cxx_compiler_flag("-mavx2" HAS_AVX2)

if(HAS_MARCH_NATIVE)
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -march=native")
endif()
if(HAS_AVX2)
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -mavx2")
endif()

# Debug: assertions + debug info
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g")

# Warnings
add_compile_options(-Wall -Wextra -Wpedantic)

# Library definition
add_library(mpmc_logger
    src/mmap_sink.cpp
    src/formatter.cpp
    src/logger.cpp
    src/timestamp.cpp
)

target_include_directories(mpmc_logger
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# Link pthreads
find_package(Threads REQUIRED)
target_link_libraries(mpmc_logger PUBLIC Threads::Threads)

# Sanitizer options
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)

if(ENABLE_TSAN)
    message(STATUS "ThreadSanitizer ENABLED")
    add_compile_options(-fsanitize=thread -g -O1)
    add_link_options(-fsanitize=thread)
endif()

if(ENABLE_ASAN)
    message(STATUS "AddressSanitizer ENABLED")
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g -O1)
    add_link_options(-fsanitize=address)
endif()

# Tests (GTest)
find_package(GTest REQUIRED)

enable_testing()

# Discover test source files
file(GLOB TEST_SOURCES test/*.cpp)

foreach(test_src ${TEST_SOURCES})
    get_filename_component(test_name ${test_src} NAME_WE)
    add_executable(${test_name} ${test_src})
    target_link_libraries(${test_name}
        PRIVATE
            mpmc_logger
            GTest::gtest_main
            Threads::Threads
    )
    add_test(NAME ${test_name} COMMAND ${test_name})

    # Set test timeout (stress tests may take longer)
    if(test_name STREQUAL "test_stress")
        set_tests_properties(${test_name} PROPERTIES TIMEOUT 120)
    else()
        set_tests_properties(${test_name} PROPERTIES TIMEOUT 30)
    endif()
endforeach()

# Benchmarks (Google Benchmark)
option(BUILD_BENCHMARKS "Build benchmark targets" ON)

if(BUILD_BENCHMARKS)
    find_package(benchmark REQUIRED)

    file(GLOB BENCH_SOURCES bench/*.cpp)

    foreach(bench_src ${BENCH_SOURCES})
        get_filename_component(bench_name ${bench_src} NAME_WE)
        add_executable(${bench_name} ${bench_src})
        target_link_libraries(${bench_name}
            PRIVATE
                mpmc_logger
                benchmark::benchmark
                Threads::Threads
        )
    endforeach()
endif()

# Tools
add_executable(log_reader tools/log_reader.cpp)
target_link_libraries(log_reader PRIVATE mpmc_logger)

add_executable(log_stats tools/log_stats.cpp)
target_link_libraries(log_stats PRIVATE mpmc_logger)

# Build configuration summary
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "TSAN: ${ENABLE_TSAN}")
message(STATUS "ASAN: ${ENABLE_ASAN}")
message(STATUS "Benchmarks: ${BUILD_BENCHMARKS}")
message(STATUS "AVX2: ${HAS_AVX2}")
message(STATUS "march=native: ${HAS_MARCH_NATIVE}")
