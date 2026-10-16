#pragma once
/// @file metrics.hpp
/// @brief Lightweight counters for query-execution telemetry.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>

namespace snowquery {

/// Accumulated metrics for a single query execution.
struct QueryMetrics {
    std::string query_text;

    size_t rows_examined  = 0;
    size_t rows_returned  = 0;
    size_t blocks_examined = 0;
    size_t blocks_skipped  = 0;
    size_t threads_used    = 1;

    std::chrono::microseconds total_time{0};

    /// Total elapsed time in milliseconds.
    double total_time_ms() const { return total_time.count() / 1000.0; }
    /// Total elapsed time in seconds.
    double total_time_s() const { return total_time.count() / 1000000.0; }

    /// Pretty-print the metrics.
    void print(std::ostream& out) const;
};

/// RAII timer that writes elapsed time into a std::chrono::microseconds ref.
class ScopedTimer {
public:
    explicit ScopedTimer(std::chrono::microseconds& target);
    ~ScopedTimer();
    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;
private:
    std::chrono::microseconds& target_;
    std::chrono::high_resolution_clock::time_point start_;
};

}  // namespace snowquery
