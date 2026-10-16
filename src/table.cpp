/// @file table.cpp
/// @brief Implementation of the Table class.

#include "snowquery/table.hpp"

#include <algorithm>
#include <charconv>
#include <iomanip>
#include <sstream>

namespace snowquery {

// ── DataType helpers ──────────────────────────────────────────────────

std::string data_type_name(DataType dt) {
    switch (dt) {
        case DataType::Null:    return "NULL";
        case DataType::Integer: return "INTEGER";
        case DataType::Float:   return "FLOAT";
        case DataType::Text:    return "TEXT";
    }
    return "UNKNOWN";
}

// ── Type inference ────────────────────────────────────────────────────

/// Try to parse a string as an integer.
static bool is_integer(const std::string& s) {
    if (s.empty()) return false;
    int64_t val{};
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), val);
    return ec == std::errc{} && ptr == s.data() + s.size();
}

/// Try to parse a string as a floating-point number.
/// We fall back to strtod since from_chars for float is not universally
/// available in all standard-library implementations.
static bool is_float(const std::string& s) {
    if (s.empty()) return false;
    char* end = nullptr;
    std::strtod(s.c_str(), &end);
    return end == s.c_str() + s.size();
}

DataType infer_type(const std::vector<std::string>& samples) {
    bool all_empty = true;
    bool all_int   = true;
    bool all_float = true;

    for (const auto& s : samples) {
        if (s.empty()) continue;  // nulls are compatible with any type
        all_empty = false;
        if (!is_integer(s)) all_int = false;
        if (!is_float(s))   all_float = false;
    }

    if (all_empty)          return DataType::Null;
    if (all_int)            return DataType::Integer;
    if (all_float)          return DataType::Float;
    return DataType::Text;
}

// ── Value parsing ─────────────────────────────────────────────────────

Value parse_value(const std::string& raw, DataType hint) {
    if (raw.empty()) return std::monostate{};  // NULL

    switch (hint) {
        case DataType::Integer: {
            int64_t val{};
            auto [ptr, ec] = std::from_chars(raw.data(),
                                             raw.data() + raw.size(), val);
            if (ec == std::errc{} && ptr == raw.data() + raw.size())
                return val;
            // Fall through to text on failure.
            return raw;
        }
        case DataType::Float: {
            char* end = nullptr;
            double val = std::strtod(raw.c_str(), &end);
            if (end == raw.c_str() + raw.size())
                return val;
            return raw;
        }
        case DataType::Null:
        case DataType::Text:
        default:
            return raw;
    }
}

// ── Table ─────────────────────────────────────────────────────────────

Table::Table(std::string name, std::vector<ColumnDef> schema)
    : name_(std::move(name)), schema_(std::move(schema)) {}

Table::Table(std::string name, std::vector<ColumnDef> schema, std::vector<Row> rows)
    : name_(std::move(name)), schema_(std::move(schema)), rows_(std::move(rows)) {}

void Table::add_row(Row row) {
    rows_.push_back(std::move(row));
}

int Table::column_index(const std::string& col_name) const {
    for (size_t i = 0; i < schema_.size(); ++i) {
        // Case-insensitive comparison.
        auto a = schema_[i].name;
        auto b = col_name;
        std::transform(a.begin(), a.end(), a.begin(), ::tolower);
        std::transform(b.begin(), b.end(), b.begin(), ::tolower);
        if (a == b) return static_cast<int>(i);
    }
    return -1;
}

Table Table::from_csv(const std::string& table_name,
                      const std::vector<std::string>& headers,
                      const std::vector<std::vector<std::string>>& rows)
{
    const size_t ncols = headers.size();

    // ── 1. Infer column types from data samples ──────────────────
    std::vector<ColumnDef> schema(ncols);
    for (size_t c = 0; c < ncols; ++c) {
        schema[c].name = headers[c];

        // Collect up to 100 samples for type inference.
        std::vector<std::string> samples;
        samples.reserve(std::min(rows.size(), size_t{100}));
        for (size_t r = 0; r < rows.size() && samples.size() < 100; ++r) {
            if (c < rows[r].size()) samples.push_back(rows[r][c]);
        }
        schema[c].type = infer_type(samples);
    }

    // ── 2. Build Table ───────────────────────────────────────────
    Table table(table_name, schema);
    table.rows_.reserve(rows.size());

    for (const auto& raw_row : rows) {
        Row row(ncols);
        for (size_t c = 0; c < ncols; ++c) {
            const std::string& cell = (c < raw_row.size()) ? raw_row[c] : "";
            row[c] = parse_value(cell, schema[c].type);
        }
        table.rows_.push_back(std::move(row));
    }

    return table;
}

// ── Pretty-printing ───────────────────────────────────────────────────

/// Format a Value for display.
static std::string format_value(const Value& v) {
    struct Visitor {
        std::string operator()(std::monostate) const { return "NULL"; }
        std::string operator()(int64_t i)      const { return std::to_string(i); }
        std::string operator()(double d)       const {
            std::ostringstream os;
            os << std::fixed << std::setprecision(2) << d;
            return os.str();
        }
        std::string operator()(const std::string& s) const { return s; }
    };
    return std::visit(Visitor{}, v);
}

void Table::print(std::ostream& out, size_t max_rows) const {
    // ── Compute column widths ────────────────────────────────────
    std::vector<size_t> widths(schema_.size());
    for (size_t c = 0; c < schema_.size(); ++c) {
        widths[c] = schema_[c].name.size();
    }
    size_t rows_to_show = std::min(rows_.size(), max_rows);
    for (size_t r = 0; r < rows_to_show; ++r) {
        for (size_t c = 0; c < schema_.size(); ++c) {
            widths[c] = std::max(widths[c], format_value(rows_[r][c]).size());
        }
    }

    // ── Header ───────────────────────────────────────────────────
    auto print_separator = [&]() {
        out << '+';
        for (size_t c = 0; c < schema_.size(); ++c)
            out << std::string(widths[c] + 2, '-') << '+';
        out << '\n';
    };

    print_separator();
    out << '|';
    for (size_t c = 0; c < schema_.size(); ++c) {
        out << ' ' << std::left << std::setw(static_cast<int>(widths[c]))
            << schema_[c].name << " |";
    }
    out << '\n';
    print_separator();

    // ── Data rows ────────────────────────────────────────────────
    for (size_t r = 0; r < rows_to_show; ++r) {
        out << '|';
        for (size_t c = 0; c < schema_.size(); ++c) {
            out << ' ' << std::left << std::setw(static_cast<int>(widths[c]))
                << format_value(rows_[r][c]) << " |";
        }
        out << '\n';
    }
    print_separator();

    out << rows_to_show << " of " << rows_.size() << " rows shown.\n";
    out << "Schema: ";
    for (size_t c = 0; c < schema_.size(); ++c) {
        if (c > 0) out << ", ";
        out << schema_[c].name << " (" << data_type_name(schema_[c].type) << ")";
    }
    out << "\n";
}

}  // namespace snowquery
