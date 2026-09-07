#include "mpmc_logger/timestamp.hpp"
#include <thread>
#include <chrono>

namespace mpmc_logger {

TscCalibration calibrate_tsc() {
    TscCalibration cal{};
    cal.available = false;
    cal.ticks_per_ns = 1.0;

#ifdef __x86_64__
    // Calibrate by measuring TSC ticks over a known wall-clock interval.
    // We use a 10ms spin to get a reasonable sample.
    constexpr int CALIBRATION_MS = 10;
    constexpr int NUM_SAMPLES    = 5;

    double total_ticks_per_ns = 0.0;

    for (int sample = 0; sample < NUM_SAMPLES; ++sample) {
        uint64_t start_ns  = now_ns();
        uint64_t start_tsc = rdtsc();

        // Spin for CALIBRATION_MS milliseconds
        auto deadline = std::chrono::steady_clock::now()
                      + std::chrono::milliseconds(CALIBRATION_MS);
        while (std::chrono::steady_clock::now() < deadline) {
            // busy-wait
        }

        uint64_t end_tsc = rdtsc();
        uint64_t end_ns  = now_ns();

        uint64_t delta_tsc = end_tsc - start_tsc;
        uint64_t delta_ns  = end_ns - start_ns;

        if (delta_ns > 0) {
            total_ticks_per_ns += static_cast<double>(delta_tsc)
                                / static_cast<double>(delta_ns);
        }
    }

    cal.ticks_per_ns = total_ticks_per_ns / NUM_SAMPLES;
    cal.available = (cal.ticks_per_ns > 0.1 && cal.ticks_per_ns < 100.0);
#endif

    return cal;
}

} // namespace mpmc_logger
