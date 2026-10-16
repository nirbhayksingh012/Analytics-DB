/// @file metrics.cpp
/// @brief Implementation of query metrics and the scoped timer.

#include "snowquery/metrics.hpp"

#include <iomanip>

namespace snowquery {

void QueryMetrics::print(std::ostream& out) const {
    out << "── Query Metrics ──────────────────────────────────\n";
    if (!query_text.empty()) {
        out << "  Query:          " << query_text << "\n";
    }
    out << "  Rows examined:  " << rows_examined  << "\n"
        << "  Rows returned:  " << rows_returned  << "\n"
        << "  Blocks examined:" << blocks_examined << "\n"
        << "  Blocks skipped: " << blocks_skipped  << "\n"
        << "  Threads used:   " << threads_used    << "\n";

    double ms = static_cast<double>(total_time.count()) / 1000.0;
    out << "  Total time:     " << std::fixed << std::setprecision(3)
        << ms << " ms\n";
    out << "───────────────────────────────────────────────────\n";
}

ScopedTimer::ScopedTimer(std::chrono::microseconds& target)
    : target_(target)
    , start_(std::chrono::high_resolution_clock::now())
{}

ScopedTimer::~ScopedTimer() {
    auto end = std::chrono::high_resolution_clock::now();
    target_ = std::chrono::duration_cast<std::chrono::microseconds>(end - start_);
}

}  // namespace snowquery
