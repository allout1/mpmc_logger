# A simple alternative to CMake for building the core benchmarks
CXX = g++
CXXFLAGS = -std=c++20 -O3 -DNDEBUG -Wall -Wextra -Iinclude -I/opt/miniconda3/include -I/opt/homebrew/include
LDFLAGS = -L/opt/miniconda3/lib -L/opt/homebrew/lib -pthread -lbenchmark

# Source files for the core library
LIB_SRCS = src/logger.cpp src/mmap_sink.cpp src/formatter.cpp src/timestamp.cpp
LIB_OBJS = $(LIB_SRCS:.cpp=.o)

all: bench_rdtsc bench_logger_e2e

# Compile object files
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Build benchmarks
bench_rdtsc: bench/bench_rdtsc.cpp $(LIB_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

bench_logger_e2e: bench/bench_logger_e2e.cpp $(LIB_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

clean:
	rm -f $(LIB_OBJS) bench_rdtsc bench_logger_e2e

.PHONY: all clean
