/// @file column_store.cpp
/// @brief Columnar storage implementation.

#include "snowquery/column_store.hpp"

#include <algorithm>
#include <iostream>

namespace snowquery {

ColumnStore ColumnStore::from_table(const Table& table) {
    ColumnStore cs;
    cs.name_ = table.name();

    const auto& schema = table.schema();
    cs.columns_.resize(schema.size());

    // Transpose: row-oriented → column-oriented.
    for (size_t c = 0; c < schema.size(); ++c) {
        cs.columns_[c].def = schema[c];
        cs.columns_[c].data.reserve(table.row_count());
    }

    for (const auto& row : table.rows()) {
        for (size_t c = 0; c < schema.size(); ++c) {
            cs.columns_[c].data.push_back(row[c]);
        }
    }

    return cs;
}

size_t ColumnStore::row_count() const {
    if (columns_.empty()) return 0;
    return columns_[0].data.size();
}

int ColumnStore::column_index(const std::string& col_name) const {
    for (size_t i = 0; i < columns_.size(); ++i) {
        // Case-insensitive compare.
        auto a = columns_[i].def.name;
        auto b = col_name;
        std::transform(a.begin(), a.end(), a.begin(), ::tolower);
        std::transform(b.begin(), b.end(), b.begin(), ::tolower);
        if (a == b) return static_cast<int>(i);
    }
    return -1;
}

void ColumnStore::print_info(std::ostream& out) const {
    out << "ColumnStore '" << name_ << "': "
        << row_count() << " rows, " << col_count() << " columns\n";
    for (const auto& col : columns_) {
        out << "  " << col.def.name
            << " (" << data_type_name(col.def.type) << ")\n";
    }
}

}  // namespace snowquery
