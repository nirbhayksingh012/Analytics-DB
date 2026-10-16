#pragma once
/// @file table.hpp
/// @brief In-memory row-oriented table with typed columns.
///
/// A Table holds a schema (column names + types) and row data.
/// Each cell is stored as a variant that can be:
///   - std::monostate  (NULL)
///   - int64_t         (integer)
///   - double          (floating-point)
///   - std::string     (text)
///
/// Tables are created from CsvData by inferring column types from the
/// first data rows.

#include <cstdint>
#include <iostream>
#include <string>
#include <variant>
#include <vector>

namespace snowquery {

/// The value stored in a single cell.
using Value = std::variant<std::monostate, int64_t, double, std::string>;

/// Supported column data types.
enum class DataType { Null, Integer, Float, Text };

/// Human-readable name for a DataType.
std::string data_type_name(DataType dt);

/// Metadata for one column.
struct ColumnDef {
    std::string name;
    DataType    type = DataType::Text;
};

/// A row is a vector of Values (one per column).
using Row = std::vector<Value>;

/// In-memory row-oriented table.
class Table {
public:
    /// Construct an empty table with a given schema.
    explicit Table(std::string name, std::vector<ColumnDef> schema);

    /// Build a table from CSV data, inferring types automatically.
    /// @param table_name  logical name (e.g. "orders")
    /// @param headers     column names
    /// @param rows        raw string rows
    static Table from_csv(const std::string& table_name,
                          const std::vector<std::string>& headers,
                          const std::vector<std::vector<std::string>>& rows);

    // ── Accessors ─────────────────────────────────────────────────
    const std::string&              name()    const { return name_; }
    const std::vector<ColumnDef>&   schema()  const { return schema_; }
    const std::vector<Row>&         rows()    const { return rows_; }
    size_t                          row_count() const { return rows_.size(); }
    size_t                          col_count() const { return schema_.size(); }

    /// Return the index of a column, or -1 if not found.
    int column_index(const std::string& col_name) const;

    /// Pretty-print the table (or first @p max_rows rows).
    void print(std::ostream& out, size_t max_rows = 50) const;

private:
    std::string              name_;
    std::vector<ColumnDef>   schema_;
    std::vector<Row>         rows_;
};

/// Try to parse a string as int64, then double; fall back to string.
Value parse_value(const std::string& raw, DataType hint);

/// Infer the best DataType for a column given sample values.
DataType infer_type(const std::vector<std::string>& samples);

}  // namespace snowquery
