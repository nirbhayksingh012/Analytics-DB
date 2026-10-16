/// @file executor.cpp
/// @brief Query executor implementation.

#include "snowquery/executor.hpp"
#include "snowquery/optimizer.hpp"
#include "snowquery/parallel.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>
#include <numeric>
#include <sstream>
#include <unordered_map>


namespace snowquery {

// ═══════════════════════════════════════════════════════════════════════
//  CATALOG
// ═══════════════════════════════════════════════════════════════════════

void Catalog::add_table(const std::string& name, const Table* table) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    tables_[lower] = table;
}

const Table* Catalog::find_table(const std::string& name) const {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    auto it = tables_.find(lower);
    return (it != tables_.end()) ? it->second : nullptr;
}

std::vector<std::string> Catalog::table_names() const {
    std::vector<std::string> names;
    names.reserve(tables_.size());
    for (const auto& [n, _] : tables_) names.push_back(n);
    std::sort(names.begin(), names.end());
    return names;
}

// ═══════════════════════════════════════════════════════════════════════
//  VALUE HELPERS
// ═══════════════════════════════════════════════════════════════════════

/// Convert a Value to double for arithmetic. Returns NaN for non-numeric.
double value_to_double(const Value& v) {
    if (auto* i = std::get_if<int64_t>(&v)) return static_cast<double>(*i);
    if (auto* d = std::get_if<double>(&v)) return *d;
    return std::numeric_limits<double>::quiet_NaN();
}

/// Convert a Value to a comparable string for sorting/display.
std::string value_to_sort_string(const Value& v) {
    struct Vis {
        std::string operator()(std::monostate) const { return ""; }
        std::string operator()(int64_t i)      const { return std::to_string(i); }
        std::string operator()(double d)        const {
            std::ostringstream os; os << d; return os.str();
        }
        std::string operator()(const std::string& s) const { return s; }
    };
    return std::visit(Vis{}, v);
}

/// Compare two Values. Returns <0, 0, >0.
int compare_values(const Value& a, const Value& b) {
    // Null handling: NULL sorts last.
    bool a_null = std::holds_alternative<std::monostate>(a);
    bool b_null = std::holds_alternative<std::monostate>(b);
    if (a_null && b_null) return 0;
    if (a_null) return 1;
    if (b_null) return -1;

    // Numeric comparison if both are numeric.
    double da = value_to_double(a);
    double db = value_to_double(b);
    if (!std::isnan(da) && !std::isnan(db)) {
        if (da < db) return -1;
        if (da > db) return 1;
        return 0;
    }

    // String comparison.
    return value_to_sort_string(a).compare(value_to_sort_string(b));
}

/// Evaluate a comparison operation.
static bool eval_comparison(BinaryExpr::Op op, int cmp) {
    switch (op) {
        case BinaryExpr::Eq:  return cmp == 0;
        case BinaryExpr::Neq: return cmp != 0;
        case BinaryExpr::Lt:  return cmp < 0;
        case BinaryExpr::Gt:  return cmp > 0;
        case BinaryExpr::Le:  return cmp <= 0;
        case BinaryExpr::Ge:  return cmp >= 0;
        default: return false;
    }
}

// ═══════════════════════════════════════════════════════════════════════
//  EXPRESSION EVALUATOR
// ═══════════════════════════════════════════════════════════════════════

/// Context for evaluating expressions against a row.
struct EvalContext {
    const std::vector<ColumnDef>* schema;
    const Row* row;
    /// For joins: combined schema + row from multiple tables.
    /// Optional secondary table schema/alias mapping.
    std::string table_alias;  // alias for the main table
};

/// Resolve a column reference to its index in the schema.
int resolve_column(const ColumnRef& ref, const std::vector<ColumnDef>& schema,
                   const std::string& /*table_alias*/) {
    std::string target = ref.column;
    std::transform(target.begin(), target.end(), target.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    for (size_t i = 0; i < schema.size(); ++i) {
        std::string col = schema[i].name;
        std::transform(col.begin(), col.end(), col.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (col == target) return static_cast<int>(i);
    }
    return -1;
}

/// Evaluate an expression to a Value.
Value eval_expr(const ExprPtr& expr, const std::vector<ColumnDef>& schema,
                const Row& row) {
    if (auto* col = dynamic_cast<ColumnRef*>(expr.get())) {
        int idx = resolve_column(*col, schema);
        if (idx < 0)
            throw SqlError("unknown column: " + col->column);
        return row[idx];
    }
    if (auto* lit = dynamic_cast<IntLit*>(expr.get()))
        return lit->value;
    if (auto* lit = dynamic_cast<FloatLit*>(expr.get()))
        return lit->value;
    if (auto* lit = dynamic_cast<StringLit*>(expr.get()))
        return lit->value;
    throw SqlError("cannot evaluate expression in this context");
}

/// Evaluate a WHERE predicate to bool.
bool eval_predicate(const ExprPtr& expr, const std::vector<ColumnDef>& schema,
                    const Row& row) {
    if (auto* bin = dynamic_cast<BinaryExpr*>(expr.get())) {
        if (bin->op == BinaryExpr::And)
            return eval_predicate(bin->left, schema, row) &&
                   eval_predicate(bin->right, schema, row);
        if (bin->op == BinaryExpr::Or)
            return eval_predicate(bin->left, schema, row) ||
                   eval_predicate(bin->right, schema, row);
        // Comparison.
        Value lv = eval_expr(bin->left, schema, row);
        Value rv = eval_expr(bin->right, schema, row);
        int cmp = compare_values(lv, rv);
        return eval_comparison(bin->op, cmp);
    }
    throw SqlError("invalid predicate expression");
}

// ═══════════════════════════════════════════════════════════════════════
//  GROUP KEY
// ═══════════════════════════════════════════════════════════════════════

/// Convert a vector of Values to a string key for grouping.
static std::string group_key(const std::vector<Value>& vals) {
    std::string key;
    for (size_t i = 0; i < vals.size(); ++i) {
        if (i > 0) key += '\x01';  // separator
        key += value_to_sort_string(vals[i]);
    }
    return key;
}

// ═══════════════════════════════════════════════════════════════════════
//  AGGREGATION STATE
// ═══════════════════════════════════════════════════════════════════════

struct AggState {
    AggFunc func;
    int64_t count = 0;
    double  sum   = 0.0;
    Value   min_val = std::monostate{};
    Value   max_val = std::monostate{};

    void accumulate(const Value& v) {
        bool is_null = std::holds_alternative<std::monostate>(v);
        if (func == AggFunc::Count) {
            if (!is_null) ++count;
            else ++count; // COUNT(*) counts all rows incl. null
            return;
        }
        if (is_null) return;  // skip nulls for SUM/MIN/MAX
        ++count;
        double d = value_to_double(v);
        if (func == AggFunc::Sum) {
            sum += d;
        } else if (func == AggFunc::Min) {
            if (count == 1 || compare_values(v, min_val) < 0) min_val = v;
        } else if (func == AggFunc::Max) {
            if (count == 1 || compare_values(v, max_val) > 0) max_val = v;
        }
    }

    Value result() const {
        if (func == AggFunc::Count) return count;
        if (count == 0) return std::monostate{};
        if (func == AggFunc::Sum) return sum;
        if (func == AggFunc::Min) return min_val;
        if (func == AggFunc::Max) return max_val;
        return std::monostate{};
    }
};



// ═══════════════════════════════════════════════════════════════════════
//  MAIN EXECUTOR
// ═══════════════════════════════════════════════════════════════════════

ExecResult execute_stmt(const SelectStmt& stmt, const Catalog& catalog,
                        const std::string& sql_text) {
    QueryMetrics metrics;
    metrics.query_text = sql_text;

    ScopedTimer timer(metrics.total_time);

    // ── Resolve the FROM table ──────────────────────────────────
    const Table* table = catalog.find_table(stmt.from_table);
    if (!table) {
        throw SqlError("unknown table: " + stmt.from_table);
    }

    // ── EXPLAIN shortcut ────────────────────────────────────────
    if (stmt.is_explain) {
        auto plan = QueryOptimizer::build_logical_plan(stmt);
        auto opt_plan = QueryOptimizer::optimize(plan, &catalog);
        std::string explain = QueryOptimizer::explain_plan(opt_plan, &catalog);
        return {Table("explain", {}), metrics, explain};
    }

    // ── Handle JOINs ────────────────────────────────────────────
    // Build a combined working schema + rows.
    std::vector<ColumnDef> work_schema = table->schema();
    std::vector<Row> work_rows = table->rows();
    metrics.rows_examined = table->row_count();

    for (const auto& join : stmt.joins) {
        const Table* right_table = catalog.find_table(join.table_name);
        if (!right_table)
            throw SqlError("unknown table in JOIN: " + join.table_name);

        // Build combined schema.
        std::vector<ColumnDef> combined_schema = work_schema;
        // Prefix right table columns if there are name collisions.
        for (const auto& col : right_table->schema()) {
            ColumnDef cd = col;
            // Check for name collision.
            bool collision = false;
            for (const auto& existing : work_schema) {
                std::string a = existing.name, b = cd.name;
                std::transform(a.begin(), a.end(), a.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                std::transform(b.begin(), b.end(), b.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                if (a == b) { collision = true; break; }
            }
            if (collision) {
                std::string prefix = join.alias.empty() ? join.table_name : join.alias;
                cd.name = prefix + "_" + cd.name;
            }
            combined_schema.push_back(cd);
        }

        // Hash join implementation.
        if (!join.on_condition)
            throw SqlError("JOIN requires an ON condition");

        // Extract join keys from ON condition (simple a.col = b.col).
        auto* bin = dynamic_cast<BinaryExpr*>(join.on_condition.get());
        if (!bin || bin->op != BinaryExpr::Eq)
            throw SqlError("JOIN ON condition must be an equality comparison");

        auto* left_ref = dynamic_cast<ColumnRef*>(bin->left.get());
        auto* right_ref = dynamic_cast<ColumnRef*>(bin->right.get());
        if (!left_ref || !right_ref)
            throw SqlError("JOIN ON must compare two columns");

        // Determine which side refers to which table.
        int left_idx = resolve_column(*left_ref, work_schema);
        int right_idx = -1;
        if (left_idx < 0) {
            // Try swapping: left might refer to right table.
            left_idx = resolve_column(*right_ref, work_schema);
            right_idx = resolve_column(*left_ref, right_table->schema());
        } else {
            right_idx = resolve_column(*right_ref, right_table->schema());
        }
        if (left_idx < 0 || right_idx < 0)
            throw SqlError("cannot resolve JOIN columns");

        // Build hash table on the right (smaller) side.
        std::unordered_multimap<std::string, size_t> hash_table;
        for (size_t r = 0; r < right_table->row_count(); ++r) {
            std::string key = value_to_sort_string(right_table->rows()[r][right_idx]);
            hash_table.emplace(key, r);
        }
        metrics.rows_examined += right_table->row_count();

        // Probe with left rows.
        std::vector<Row> joined_rows;
        for (const auto& left_row : work_rows) {
            std::string key = value_to_sort_string(left_row[left_idx]);
            auto [begin, end] = hash_table.equal_range(key);
            for (auto it = begin; it != end; ++it) {
                Row combined = left_row;
                const Row& right_row = right_table->rows()[it->second];
                combined.insert(combined.end(), right_row.begin(), right_row.end());
                joined_rows.push_back(std::move(combined));
            }
        }

        work_schema = std::move(combined_schema);
        work_rows = std::move(joined_rows);
    }

    // ── WHERE filter ────────────────────────────────────────────
    std::vector<Row> filtered;
    if (work_rows.size() >= 500 && stmt.where_clause) {
        filtered = ParallelEngine::parallel_scan_filter(work_schema, work_rows, stmt.where_clause, {}, &metrics);
    } else if (stmt.where_clause) {
        filtered.reserve(work_rows.size());
        for (const auto& row : work_rows) {
            if (eval_predicate(stmt.where_clause, work_schema, row))
                filtered.push_back(row);
        }
    } else {
        filtered = std::move(work_rows);
    }

    // ── Detect aggregation vs plain SELECT ──────────────────────
    bool has_agg = false;
    bool has_group_by = !stmt.group_by.empty();
    for (const auto& item : stmt.select_list) {
        if (dynamic_cast<AggExpr*>(item.expr.get())) { has_agg = true; break; }
    }

    std::vector<ColumnDef> result_schema;
    std::vector<Row> result_rows;

    if (has_agg || has_group_by) {
        if (filtered.size() >= 500) {
            Table agg_res = ParallelEngine::parallel_aggregate(work_schema, filtered, stmt.group_by, stmt.select_list, &metrics);
            result_schema = agg_res.schema();
            result_rows = agg_res.rows();
        } else {
            // ── GROUP BY + Aggregation ──────────────────────────────
            // Resolve group-by column indices.
            std::vector<int> group_col_indices;
            for (const auto& gb : stmt.group_by) {
                auto* col = dynamic_cast<ColumnRef*>(gb.get());
                if (!col) throw SqlError("GROUP BY requires column references");
                int idx = resolve_column(*col, work_schema);
                if (idx < 0) throw SqlError("unknown column in GROUP BY: " + col->column);
                group_col_indices.push_back(idx);
            }



        // Group the rows.
        std::map<std::string, std::vector<const Row*>> groups;
        std::vector<std::string> group_order;  // maintain insertion order
        for (const auto& row : filtered) {
            std::vector<Value> key_vals;
            for (int idx : group_col_indices) key_vals.push_back(row[idx]);
            std::string key = group_key(key_vals);
            if (groups.find(key) == groups.end()) group_order.push_back(key);
            groups[key].push_back(&row);
        }

        // If no GROUP BY but has aggregates, everything is one group.
        if (group_col_indices.empty() && groups.empty()) {
            groups[""].clear();
            group_order.push_back("");
        }

        // Build result schema and compute aggregates.
        for (const auto& item : stmt.select_list) {
            ColumnDef cd;
            if (auto* agg = dynamic_cast<AggExpr*>(item.expr.get())) {
                cd.name = item.alias.empty() ? expr_to_string(item.expr) : item.alias;
                cd.type = (agg->func == AggFunc::Count) ? DataType::Integer : DataType::Float;
            } else if (auto* col = dynamic_cast<ColumnRef*>(item.expr.get())) {
                int idx = resolve_column(*col, work_schema);
                if (idx < 0) throw SqlError("unknown column: " + col->column);
                cd.name = item.alias.empty() ? col->column : item.alias;
                cd.type = work_schema[idx].type;
            } else {
                throw SqlError("unsupported expression in SELECT with GROUP BY");
            }
            result_schema.push_back(cd);
        }

        // Compute each group.
        for (const auto& key : group_order) {
            const auto& group_rows = groups[key];
            Row result_row;

            for (const auto& item : stmt.select_list) {
                if (auto* agg = dynamic_cast<AggExpr*>(item.expr.get())) {
                    AggState state;
                    state.func = agg->func;
                    bool is_count_star = (dynamic_cast<StarExpr*>(agg->arg.get()) != nullptr);

                    for (const Row* row : group_rows) {
                        if (is_count_star) {
                            state.count++;
                        } else {
                            auto* arg_col = dynamic_cast<ColumnRef*>(agg->arg.get());
                            if (!arg_col) throw SqlError("aggregate argument must be a column or *");
                            int idx = resolve_column(*arg_col, work_schema);
                            if (idx < 0) throw SqlError("unknown column: " + arg_col->column);
                            state.accumulate((*row)[idx]);
                        }
                    }
                    if (is_count_star && agg->func == AggFunc::Count)
                        result_row.push_back(state.count);
                    else
                        result_row.push_back(state.result());
                } else if (auto* col = dynamic_cast<ColumnRef*>(item.expr.get())) {
                    // Non-aggregate column in GROUP BY result — use first row's value.
                    int idx = resolve_column(*col, work_schema);
                    if (!group_rows.empty()) {
                        result_row.push_back((*group_rows[0])[idx]);
                    } else {
                        result_row.push_back(std::monostate{});
                    }
                }
            }
            result_rows.push_back(std::move(result_row));
        }
        }
    } else {
        // ── Plain SELECT (no aggregation) ───────────────────────
        // Resolve selected columns.
        bool select_star = false;
        std::vector<int> col_indices;

        for (const auto& item : stmt.select_list) {
            if (dynamic_cast<StarExpr*>(item.expr.get())) {
                select_star = true;
                break;
            }
            auto* col = dynamic_cast<ColumnRef*>(item.expr.get());
            if (!col) throw SqlError("unsupported expression in SELECT");
            int idx = resolve_column(*col, work_schema);
            if (idx < 0) throw SqlError("unknown column: " + col->column);
            col_indices.push_back(idx);
        }

        if (select_star) {
            result_schema = work_schema;
            result_rows = std::move(filtered);
        } else {
            for (size_t i = 0; i < col_indices.size(); ++i) {
                ColumnDef cd = work_schema[col_indices[i]];
                if (!stmt.select_list[i].alias.empty())
                    cd.name = stmt.select_list[i].alias;
                result_schema.push_back(cd);
            }
            result_rows.reserve(filtered.size());
            for (const auto& row : filtered) {
                Row projected;
                projected.reserve(col_indices.size());
                for (int idx : col_indices) projected.push_back(row[idx]);
                result_rows.push_back(std::move(projected));
            }
        }
    }

    // ── ORDER BY ────────────────────────────────────────────────
    if (!stmt.order_by.empty()) {
        // Resolve ORDER BY column indices against the RESULT schema.
        struct SortKey {
            int idx;
            SortDir dir;
        };
        std::vector<SortKey> sort_keys;
        for (const auto& ob : stmt.order_by) {
            int idx = -1;
            if (auto* col = dynamic_cast<ColumnRef*>(ob.expr.get())) {
                idx = resolve_column(*col, result_schema);
            } else if (dynamic_cast<AggExpr*>(ob.expr.get())) {
                // Match aggregate by name.
                std::string agg_name = expr_to_string(ob.expr);
                for (size_t i = 0; i < result_schema.size(); ++i) {
                    if (result_schema[i].name == agg_name) { idx = static_cast<int>(i); break; }
                }
            }
            if (idx < 0) throw SqlError("ORDER BY column not found in result");
            sort_keys.push_back({idx, ob.dir});
        }

        std::sort(result_rows.begin(), result_rows.end(),
                  [&sort_keys](const Row& a, const Row& b) {
            for (const auto& sk : sort_keys) {
                int cmp = compare_values(a[sk.idx], b[sk.idx]);
                if (cmp != 0)
                    return (sk.dir == SortDir::Asc) ? (cmp < 0) : (cmp > 0);
            }
            return false;
        });
    }

    // ── LIMIT ───────────────────────────────────────────────────
    if (stmt.limit && static_cast<size_t>(*stmt.limit) < result_rows.size()) {
        result_rows.resize(*stmt.limit);
    }

    // ── Build result table ──────────────────────────────────────
    metrics.rows_returned = result_rows.size();
    if (stmt.joins.empty())
        metrics.rows_examined = table->row_count();

    Table result("result", std::move(result_schema), std::move(result_rows));
    return {std::move(result), metrics, ""};
}

ExecResult execute_query(const std::string& sql, const Catalog& catalog) {
    auto stmt = parse_sql(sql);
    return execute_stmt(stmt, catalog, sql);
}

}  // namespace snowquery
