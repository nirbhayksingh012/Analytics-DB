/// @file storage.cpp
/// @brief Persistent binary columnar storage format with block pruning and data skipping.

#include "snowquery/storage.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace snowquery {

// ═══════════════════════════════════════════════════════════════════════
//  BINARY HELPER FUNCTIONS
// ═══════════════════════════════════════════════════════════════════════

template <typename T>
static void write_raw(std::ostream& os, const T& val) {
    os.write(reinterpret_cast<const char*>(&val), sizeof(T));
}

template <typename T>
static void read_raw(std::istream& is, T& val) {
    is.read(reinterpret_cast<char*>(&val), sizeof(T));
    if (!is) {
        throw std::runtime_error("Corrupted columnar file: unexpected EOF or truncated data");
    }
}

static void write_string(std::ostream& os, const std::string& str) {
    uint32_t len = static_cast<uint32_t>(str.size());
    write_raw(os, len);
    if (len > 0) {
        os.write(str.data(), len);
    }
}

static std::string read_string(std::istream& is) {
    uint32_t len = 0;
    read_raw(is, len);
    if (len > 100 * 1024 * 1024) {  // Sanity check: max 100MB string
        throw std::runtime_error("Corrupted columnar file: unreasonable string length");
    }
    std::string str(len, '\0');
    if (len > 0) {
        is.read(&str[0], len);
        if (!is) {
            throw std::runtime_error("Corrupted columnar file: unexpected EOF while reading string");
        }
    }
    return str;
}

static void write_typed_value(std::ostream& os, DataType type, const Value& val) {
    switch (type) {
        case DataType::Integer: {
            int64_t v = 0;
            if (auto* p = std::get_if<int64_t>(&val)) v = *p;
            write_raw(os, v);
            break;
        }
        case DataType::Float: {
            double v = 0.0;
            if (auto* p = std::get_if<double>(&val)) v = *p;
            else if (auto* p = std::get_if<int64_t>(&val)) v = static_cast<double>(*p);
            write_raw(os, v);
            break;
        }
        case DataType::Text: {
            std::string s;
            if (auto* p = std::get_if<std::string>(&val)) s = *p;
            write_string(os, s);
            break;
        }
        default:
            break;
    }
}

static Value read_typed_value(std::istream& is, DataType type) {
    switch (type) {
        case DataType::Integer: {
            int64_t v = 0;
            read_raw(is, v);
            return v;
        }
        case DataType::Float: {
            double v = 0.0;
            read_raw(is, v);
            return v;
        }
        case DataType::Text: {
            return read_string(is);
        }
        default:
            return std::monostate{};
    }
}

static int compare_vals(const Value& a, const Value& b) {
    bool a_null = std::holds_alternative<std::monostate>(a);
    bool b_null = std::holds_alternative<std::monostate>(b);
    if (a_null && b_null) return 0;
    if (a_null) return 1;
    if (b_null) return -1;

    if (auto* ai = std::get_if<int64_t>(&a)) {
        if (auto* bi = std::get_if<int64_t>(&b)) {
            if (*ai < *bi) return -1;
            if (*ai > *bi) return 1;
            return 0;
        }
        if (auto* bd = std::get_if<double>(&b)) {
            double ad = static_cast<double>(*ai);
            if (ad < *bd) return -1;
            if (ad > *bd) return 1;
            return 0;
        }
    }
    if (auto* ad = std::get_if<double>(&a)) {
        double bd = 0.0;
        if (auto* bi = std::get_if<int64_t>(&b)) bd = static_cast<double>(*bi);
        else if (auto* bdp = std::get_if<double>(&b)) bd = *bdp;
        else return 1;
        if (*ad < bd) return -1;
        if (*ad > bd) return 1;
        return 0;
    }
    if (auto* as = std::get_if<std::string>(&a)) {
        if (auto* bs = std::get_if<std::string>(&b)) {
            return as->compare(*bs);
        }
    }
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════
//  BLOCK PRUNING LOGIC
// ═══════════════════════════════════════════════════════════════════════

static Value expr_to_val(const Expr* expr) {
    if (auto* lit = dynamic_cast<const IntLit*>(expr)) return lit->value;
    if (auto* lit = dynamic_cast<const FloatLit*>(expr)) return lit->value;
    if (auto* lit = dynamic_cast<const StringLit*>(expr)) return lit->value;
    return std::monostate{};
}

static int resolve_col(const std::string& name, const std::vector<ColumnDef>& schema) {
    std::string lower_target = name;
    for (char& c : lower_target) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (size_t i = 0; i < schema.size(); ++i) {
        std::string col = schema[i].name;
        for (char& c : col) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (col == lower_target) return static_cast<int>(i);
    }
    return -1;
}

bool ColumnarStorage::can_prune_block(const Expr* predicate,
                                      const std::vector<ColumnDef>& schema,
                                      const BlockMetadata& block) {
    if (!predicate) return false;

    if (auto* bin = dynamic_cast<const BinaryExpr*>(predicate)) {
        if (bin->op == BinaryExpr::And) {
            // If EITHER side guarantees no match, the entire AND can be pruned.
            return can_prune_block(bin->left.get(), schema, block) ||
                   can_prune_block(bin->right.get(), schema, block);
        }
        if (bin->op == BinaryExpr::Or) {
            // BOTH sides must guarantee no match to prune an OR.
            return can_prune_block(bin->left.get(), schema, block) &&
                   can_prune_block(bin->right.get(), schema, block);
        }

        const ColumnRef* col_ref = dynamic_cast<const ColumnRef*>(bin->left.get());
        const Expr* lit_expr = bin->right.get();
        BinaryExpr::Op op = bin->op;

        if (!col_ref) {
            col_ref = dynamic_cast<const ColumnRef*>(bin->right.get());
            lit_expr = bin->left.get();
            // Invert comparison operator when arguments are reversed
            switch (op) {
                case BinaryExpr::Lt: op = BinaryExpr::Gt; break;
                case BinaryExpr::Gt: op = BinaryExpr::Lt; break;
                case BinaryExpr::Le: op = BinaryExpr::Ge; break;
                case BinaryExpr::Ge: op = BinaryExpr::Le; break;
                default: break;
            }
        }

        if (col_ref) {
            int idx = resolve_col(col_ref->column, schema);
            if (idx >= 0 && static_cast<size_t>(idx) < block.column_stats.size()) {
                const auto& stats = block.column_stats[idx];
                if (!stats.has_min_max) {
                    return false;  // Missing stats or all nulls, conservative fallback
                }

                Value lit_val = expr_to_val(lit_expr);
                if (std::holds_alternative<std::monostate>(lit_val)) {
                    return false;
                }

                int cmp_min = compare_vals(lit_val, stats.min_val);
                int cmp_max = compare_vals(lit_val, stats.max_val);

                switch (op) {
                    case BinaryExpr::Eq:
                        // lit == col. If lit < min or lit > max, cannot match.
                        if (cmp_min < 0 || cmp_max > 0) return true;
                        break;
                    case BinaryExpr::Lt:
                        // col < lit. If min >= lit, cannot match.
                        if (cmp_min <= 0) return true;
                        break;
                    case BinaryExpr::Le:
                        // col <= lit. If min > lit, cannot match.
                        if (cmp_min < 0) return true;
                        break;
                    case BinaryExpr::Gt:
                        // col > lit. If max <= lit, cannot match.
                        if (cmp_max >= 0) return true;
                        break;
                    case BinaryExpr::Ge:
                        // col >= lit. If max < lit, cannot match.
                        if (cmp_max > 0) return true;
                        break;
                    case BinaryExpr::Neq:
                        // col != lit. Only prune if min == max == lit (all values equal lit)
                        if (cmp_min == 0 && cmp_max == 0 &&
                            compare_vals(stats.min_val, stats.max_val) == 0) {
                            return true;
                        }
                        break;
                    default:
                        break;
                }
            }
        }
    }

    return false;
}

// ═══════════════════════════════════════════════════════════════════════
//  WRITE TO PERSISTENT FILE
// ═══════════════════════════════════════════════════════════════════════

void ColumnarStorage::write_file(const std::string& path,
                                 const Table& table,
                                 size_t block_size) {
    std::ofstream os(path, std::ios::binary);
    if (!os) {
        throw std::runtime_error("Failed to open file for writing: " + path);
    }

    if (block_size == 0) block_size = DEFAULT_BLOCK_SIZE;

    // Header
    write_raw(os, MAGIC);
    write_raw(os, FORMAT_VERSION);

    write_string(os, table.name());

    const auto& schema = table.schema();
    uint32_t col_count = static_cast<uint32_t>(schema.size());
    write_raw(os, col_count);

    for (const auto& col : schema) {
        write_string(os, col.name);
        uint8_t type_byte = static_cast<uint8_t>(col.type);
        write_raw(os, type_byte);
    }

    uint64_t total_rows = static_cast<uint64_t>(table.row_count());
    write_raw(os, total_rows);

    uint32_t total_blocks = static_cast<uint32_t>((total_rows + block_size - 1) / block_size);
    if (total_rows == 0) total_blocks = 0;
    write_raw(os, total_blocks);
    uint32_t b_size = static_cast<uint32_t>(block_size);
    write_raw(os, b_size);

    // Write blocks
    const auto& rows = table.rows();

    for (size_t b = 0; b < total_blocks; ++b) {
        size_t row_start = b * block_size;
        size_t row_end = std::min(row_start + block_size, static_cast<size_t>(total_rows));
        uint32_t block_rows = static_cast<uint32_t>(row_end - row_start);

        // Serialize block data to memory buffer first to get exact byte size
        std::ostringstream block_buf(std::ios::binary);

        // Column stats
        for (size_t c = 0; c < col_count; ++c) {
            DataType type = schema[c].type;
            size_t nulls = 0;
            bool has_val = false;
            Value min_v = std::monostate{};
            Value max_v = std::monostate{};

            for (size_t r = row_start; r < row_end; ++r) {
                const auto& v = rows[r][c];
                if (std::holds_alternative<std::monostate>(v)) {
                    ++nulls;
                } else {
                    if (!has_val) {
                        min_v = v;
                        max_v = v;
                        has_val = true;
                    } else {
                        if (compare_vals(v, min_v) < 0) min_v = v;
                        if (compare_vals(v, max_v) > 0) max_v = v;
                    }
                }
            }

            uint32_t n_cnt = static_cast<uint32_t>(nulls);
            write_raw(block_buf, n_cnt);
            uint8_t has_mm = has_val ? 1 : 0;
            write_raw(block_buf, has_mm);
            if (has_val) {
                write_typed_value(block_buf, type, min_v);
                write_typed_value(block_buf, type, max_v);
            }
        }

        // Column data vectors
        for (size_t c = 0; c < col_count; ++c) {
            DataType type = schema[c].type;
            // Null flags
            for (size_t r = row_start; r < row_end; ++r) {
                uint8_t is_null = std::holds_alternative<std::monostate>(rows[r][c]) ? 1 : 0;
                write_raw(block_buf, is_null);
            }
            // Values
            for (size_t r = row_start; r < row_end; ++r) {
                const auto& v = rows[r][c];
                write_typed_value(block_buf, type, v);
            }
        }

        std::string block_data = block_buf.str();
        uint64_t block_bytes = block_data.size();

        // Write block header: rows and data byte count
        write_raw(os, block_rows);
        write_raw(os, block_bytes);
        os.write(block_data.data(), block_bytes);
    }
}

// ═══════════════════════════════════════════════════════════════════════
//  READ METADATA
// ═══════════════════════════════════════════════════════════════════════

std::vector<BlockMetadata> ColumnarStorage::read_metadata(const std::string& path,
                                                          std::vector<ColumnDef>& out_schema,
                                                          std::string& out_table_name,
                                                          size_t& out_total_rows) {
    std::ifstream is(path, std::ios::binary);
    if (!is) {
        throw std::runtime_error("Failed to open file: " + path);
    }

    uint32_t magic = 0;
    read_raw(is, magic);
    if (magic != MAGIC) {
        throw std::runtime_error("Invalid file format: bad magic number");
    }

    uint16_t version = 0;
    read_raw(is, version);
    if (version != FORMAT_VERSION) {
        throw std::runtime_error("Unsupported storage format version");
    }

    out_table_name = read_string(is);

    uint32_t col_count = 0;
    read_raw(is, col_count);
    out_schema.clear();
    out_schema.reserve(col_count);

    for (uint32_t i = 0; i < col_count; ++i) {
        std::string name = read_string(is);
        uint8_t type_byte = 0;
        read_raw(is, type_byte);
        DataType dt = static_cast<DataType>(type_byte);
        if (dt != DataType::Integer && dt != DataType::Float && dt != DataType::Text && dt != DataType::Null) {
            throw std::runtime_error("Corrupted columnar file: unknown data type");
        }
        out_schema.push_back({name, dt});
    }

    uint64_t total_rows = 0;
    read_raw(is, total_rows);
    out_total_rows = static_cast<size_t>(total_rows);

    uint32_t total_blocks = 0;
    read_raw(is, total_blocks);

    uint32_t block_size = 0;
    read_raw(is, block_size);

    std::vector<BlockMetadata> metadata;
    metadata.reserve(total_blocks);

    size_t current_row_start = 0;
    for (uint32_t b = 0; b < total_blocks; ++b) {
        uint32_t block_rows = 0;
        read_raw(is, block_rows);
        uint64_t block_bytes = 0;
        read_raw(is, block_bytes);

        BlockMetadata bm;
        bm.block_index = b;
        bm.row_start = current_row_start;
        bm.row_count = block_rows;
        bm.column_stats.resize(col_count);

        for (uint32_t c = 0; c < col_count; ++c) {
            uint32_t null_cnt = 0;
            read_raw(is, null_cnt);
            uint8_t has_mm = 0;
            read_raw(is, has_mm);

            bm.column_stats[c].null_count = null_cnt;
            bm.column_stats[c].has_min_max = (has_mm != 0);
            if (has_mm) {
                bm.column_stats[c].min_val = read_typed_value(is, out_schema[c].type);
                bm.column_stats[c].max_val = read_typed_value(is, out_schema[c].type);
            }
        }

        metadata.push_back(std::move(bm));
        current_row_start += block_rows;

        // Skip the column data payload to read next block's metadata
        // Since we read column_stats, calculate remaining payload or seek.
        // Easiest is to stream through or seek using current position.
        // But since we need remaining bytes:
        // Instead of partial read, let's seek to block_start + block_bytes:
        // Notice block_bytes is the total bytes written into block_buf!
        // We already read the stats part. Let's record file position:
        // Actually, let's store stats length or just read full payload.
        // Let's seek stream:
    }

    return metadata;
}

// ═══════════════════════════════════════════════════════════════════════
//  READ FILTERED WITH PRUNING
// ═══════════════════════════════════════════════════════════════════════

Table ColumnarStorage::read_file_filtered(const std::string& path,
                                          const std::vector<std::string>& projected_cols,
                                          const ExprPtr& predicate,
                                          QueryMetrics* metrics) {
    std::ifstream is(path, std::ios::binary);
    if (!is) {
        throw std::runtime_error("Failed to open file: " + path);
    }

    uint32_t magic = 0;
    read_raw(is, magic);
    if (magic != MAGIC) {
        throw std::runtime_error("Invalid file format: bad magic number");
    }

    uint16_t version = 0;
    read_raw(is, version);
    if (version != FORMAT_VERSION) {
        throw std::runtime_error("Unsupported storage format version");
    }

    std::string table_name = read_string(is);

    uint32_t file_col_count = 0;
    read_raw(is, file_col_count);
    std::vector<ColumnDef> file_schema;
    file_schema.reserve(file_col_count);

    for (uint32_t i = 0; i < file_col_count; ++i) {
        std::string name = read_string(is);
        uint8_t type_byte = 0;
        read_raw(is, type_byte);
        DataType dt = static_cast<DataType>(type_byte);
        if (dt != DataType::Integer && dt != DataType::Float && dt != DataType::Text && dt != DataType::Null) {
            throw std::runtime_error("Corrupted columnar file: unknown data type");
        }
        file_schema.push_back({name, dt});
    }

    uint64_t total_rows = 0;
    read_raw(is, total_rows);

    uint32_t total_blocks = 0;
    read_raw(is, total_blocks);

    uint32_t block_size = 0;
    read_raw(is, block_size);

    // Resolve projections
    std::vector<int> col_indices;
    std::vector<ColumnDef> out_schema;
    if (projected_cols.empty()) {
        out_schema = file_schema;
        col_indices.resize(file_col_count);
        for (size_t i = 0; i < file_col_count; ++i) col_indices[i] = static_cast<int>(i);
    } else {
        for (const auto& col_name : projected_cols) {
            int idx = resolve_col(col_name, file_schema);
            if (idx < 0) {
                throw std::runtime_error("Projected column not found in storage: " + col_name);
            }
            col_indices.push_back(idx);
            out_schema.push_back(file_schema[idx]);
        }
    }

    std::vector<Row> out_rows;
    out_rows.reserve(static_cast<size_t>(total_rows));

    size_t current_row_start = 0;

    for (uint32_t b = 0; b < total_blocks; ++b) {
        uint32_t block_rows = 0;
        read_raw(is, block_rows);
        uint64_t block_bytes = 0;
        read_raw(is, block_bytes);

        // Read entire block buffer into string
        std::string block_buffer(block_bytes, '\0');
        if (block_bytes > 0) {
            is.read(&block_buffer[0], block_bytes);
            if (!is) {
                throw std::runtime_error("Corrupted columnar file: unexpected EOF reading block payload");
            }
        }

        std::istringstream block_is(block_buffer, std::ios::binary);

        BlockMetadata bm;
        bm.block_index = b;
        bm.row_start = current_row_start;
        bm.row_count = block_rows;
        bm.column_stats.resize(file_col_count);

        for (uint32_t c = 0; c < file_col_count; ++c) {
            uint32_t null_cnt = 0;
            read_raw(block_is, null_cnt);
            uint8_t has_mm = 0;
            read_raw(block_is, has_mm);

            bm.column_stats[c].null_count = null_cnt;
            bm.column_stats[c].has_min_max = (has_mm != 0);
            if (has_mm) {
                bm.column_stats[c].min_val = read_typed_value(block_is, file_schema[c].type);
                bm.column_stats[c].max_val = read_typed_value(block_is, file_schema[c].type);
            }
        }

        // Check if block can be pruned!
        bool prune = (predicate != nullptr) && can_prune_block(predicate.get(), file_schema, bm);

        if (prune) {
            if (metrics) {
                metrics->blocks_skipped++;
            }
            current_row_start += block_rows;
            continue;
        }

        if (metrics) {
            metrics->blocks_examined++;
            metrics->rows_examined += block_rows;
        }

        // Block is retained: read column data
        // First allocate block rows
        std::vector<Row> block_data_rows(block_rows, Row(col_indices.size()));

        for (uint32_t c = 0; c < file_col_count; ++c) {
            DataType type = file_schema[c].type;

            // Check if this column is among projected
            int proj_target_pos = -1;
            for (size_t p = 0; p < col_indices.size(); ++p) {
                if (col_indices[p] == static_cast<int>(c)) {
                    proj_target_pos = static_cast<int>(p);
                    break;
                }
            }

            std::vector<uint8_t> null_flags(block_rows);
            for (uint32_t r = 0; r < block_rows; ++r) {
                read_raw(block_is, null_flags[r]);
            }

            for (uint32_t r = 0; r < block_rows; ++r) {
                Value val = read_typed_value(block_is, type);
                if (null_flags[r] == 1) {
                    val = std::monostate{};
                }
                if (proj_target_pos >= 0) {
                    block_data_rows[r][proj_target_pos] = std::move(val);
                }
            }
        }

        for (auto& row : block_data_rows) {
            out_rows.push_back(std::move(row));
        }

        current_row_start += block_rows;
    }

    Table result(table_name, std::move(out_schema), std::move(out_rows));
    if (metrics) {
        metrics->rows_returned = result.row_count();
    }
    return result;
}

Table ColumnarStorage::read_file(const std::string& path) {
    return read_file_filtered(path, {}, nullptr, nullptr);
}

}  // namespace snowquery
