#pragma once
/// @file parallel.hpp
/// @brief Parallel query execution engine (parallel scans, filters, and aggregations).

#include "metrics.hpp"
#include "sql_parser.hpp"
#include "table.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace snowquery {

/// Configuration options for parallel engine.
struct ParallelConfig {
    size_t num_threads         = 0;    // 0 = hardware concurrency
    size_t min_rows_per_thread = 500;  // sequential fallback below this threshold
};

/// Vectorized batch of columnar values.
struct VectorBatch {
    size_t size = 0;
    std::vector<ColumnDef> schema;
    std::vector<std::vector<Value>> columns;  // columns[c][row]

    static VectorBatch from_rows(const std::vector<ColumnDef>& schema,
                                 const std::vector<Row>& rows,
                                 size_t start, size_t count);

    std::vector<Row> to_rows() const;
};

/// Parallel execution engine using C++20 threading facilities.
class ParallelEngine {
public:
    /// Determine effective worker thread count.
    static size_t resolve_thread_count(size_t total_rows, ParallelConfig config = {});

    /// Parallel scan and filter: evaluates predicate across parallel row chunks.
    static std::vector<Row> parallel_scan_filter(
        const std::vector<ColumnDef>& schema,
        const std::vector<Row>& rows,
        const ExprPtr& predicate,
        const std::vector<int>& projection_indices,
        QueryMetrics* metrics = nullptr,
        ParallelConfig config = {});

    /// Parallel aggregation: computes thread-local groups and aggregates, then merges.
    static Table parallel_aggregate(
        const std::vector<ColumnDef>& work_schema,
        const std::vector<Row>& filtered_rows,
        const std::vector<ExprPtr>& group_by,
        const std::vector<SelectItem>& select_list,
        QueryMetrics* metrics = nullptr,
        ParallelConfig config = {});
};

}  // namespace snowquery
