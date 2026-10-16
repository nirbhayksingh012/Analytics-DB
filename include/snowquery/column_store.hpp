#pragma once
/// @file column_store.hpp
/// @brief Columnar in-memory representation with block metadata.
///
/// Phase 1 stub — provides the public interface that later phases will
/// flesh out with typed column vectors, min/max metadata, block pruning,
/// and binary persistence.

#include "table.hpp"
#include <string>
#include <vector>

namespace snowquery {

/// A single typed column stored as a vector of Values.
struct Column {
    ColumnDef               def;
    std::vector<Value>      data;
};

/// Columnar representation of a table.
class ColumnStore {
public:
    /// Build a column store from a row-oriented Table.
    static ColumnStore from_table(const Table& table);

    const std::string&           name()    const { return name_; }
    const std::vector<Column>&   columns() const { return columns_; }
    size_t                       row_count() const;
    size_t                       col_count() const { return columns_.size(); }

    /// Return the index of a column, or -1 if not found.
    int column_index(const std::string& col_name) const;

    /// Print summary information.
    void print_info(std::ostream& out) const;

private:
    std::string          name_;
    std::vector<Column>  columns_;
};

}  // namespace snowquery
