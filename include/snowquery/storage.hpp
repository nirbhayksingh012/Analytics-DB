#pragma once
/// @file storage.hpp
/// @brief Persistent binary columnar storage format and data skipping with block pruning.

#include "metrics.hpp"
#include "sql_parser.hpp"
#include "table.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace snowquery {

/// Block-level statistics for a single column.
struct ColumnBlockStats {
    size_t null_count = 0;
    bool   has_min_max = false;
    Value  min_val = std::monostate{};
    Value  max_val = std::monostate{};
};

/// Block-level metadata across all columns.
struct BlockMetadata {
    size_t block_index = 0;
    size_t row_start   = 0;
    size_t row_count   = 0;
    std::vector<ColumnBlockStats> column_stats;
};

/// Persistent columnar file storage manager.
class ColumnarStorage {
public:
    static constexpr uint32_t MAGIC = 0x53434F4C;  // 'SCOL' in hex
    static constexpr uint16_t FORMAT_VERSION = 1;
    static constexpr size_t   DEFAULT_BLOCK_SIZE = 1024;

    /// Write a table to a persistent binary columnar file (.scol).
    static void write_file(const std::string& path,
                           const Table& table,
                           size_t block_size = DEFAULT_BLOCK_SIZE);

    /// Load an entire table from a persistent binary columnar file.
    static Table read_file(const std::string& path);

    /// Read table with projection pushdown and block pruning.
    /// If projected_cols is empty, all columns are read.
    /// If predicate is non-null, data skipping prunes non-matching blocks.
    static Table read_file_filtered(const std::string& path,
                                    const std::vector<std::string>& projected_cols,
                                    const ExprPtr& predicate = nullptr,
                                    QueryMetrics* metrics = nullptr);

    /// Inspect metadata without reading full data vectors.
    static std::vector<BlockMetadata> read_metadata(const std::string& path,
                                                    std::vector<ColumnDef>& out_schema,
                                                    std::string& out_table_name,
                                                    size_t& out_total_rows);

    /// Conservative block pruning: returns true if block is guaranteed NOT to match.
    static bool can_prune_block(const Expr* predicate,
                                const std::vector<ColumnDef>& schema,
                                const BlockMetadata& block);
};

}  // namespace snowquery
