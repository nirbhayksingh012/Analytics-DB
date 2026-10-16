/// @file parallel.cpp
/// @brief Implementation of parallel query execution (scans, filters, aggregations).

#include "snowquery/parallel.hpp"
#include "snowquery/executor.hpp"

#include <algorithm>
#include <exception>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>

namespace snowquery {

// ═══════════════════════════════════════════════════════════════════════
//  VECTOR BATCH
// ═══════════════════════════════════════════════════════════════════════

VectorBatch VectorBatch::from_rows(const std::vector<ColumnDef>& schema,
                                   const std::vector<Row>& rows,
                                   size_t start, size_t count) {
    VectorBatch vb;
    vb.schema = schema;
    vb.size = count;
    vb.columns.resize(schema.size());

    for (size_t c = 0; c < schema.size(); ++c) {
        vb.columns[c].reserve(count);
    }

    size_t end = std::min(start + count, rows.size());
    for (size_t r = start; r < end; ++r) {
        for (size_t c = 0; c < schema.size(); ++c) {
            vb.columns[c].push_back(rows[r][c]);
        }
    }

    return vb;
}

std::vector<Row> VectorBatch::to_rows() const {
    std::vector<Row> out_rows;
    out_rows.reserve(size);

    for (size_t r = 0; r < size; ++r) {
        Row row;
        row.reserve(columns.size());
        for (size_t c = 0; c < columns.size(); ++c) {
            row.push_back(columns[c][r]);
        }
        out_rows.push_back(std::move(row));
    }

    return out_rows;
}

// ═══════════════════════════════════════════════════════════════════════
//  THREAD RESOLUTION
// ═══════════════════════════════════════════════════════════════════════

size_t ParallelEngine::resolve_thread_count(size_t total_rows, ParallelConfig config) {
    if (config.num_threads > 0) return config.num_threads;

    if (total_rows < config.min_rows_per_thread) {
        return 1;  // Sequential fallback for small workloads
    }

    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;

    size_t max_threads = (total_rows + config.min_rows_per_thread - 1) / config.min_rows_per_thread;
    return std::max(size_t{1}, std::min(static_cast<size_t>(hw), max_threads));
}

// ═══════════════════════════════════════════════════════════════════════
//  PARALLEL SCAN & FILTER
// ═══════════════════════════════════════════════════════════════════════

std::vector<Row> ParallelEngine::parallel_scan_filter(
    const std::vector<ColumnDef>& schema,
    const std::vector<Row>& rows,
    const ExprPtr& predicate,
    const std::vector<int>& projection_indices,
    QueryMetrics* metrics,
    ParallelConfig config) {

    size_t n = rows.size();
    size_t T = resolve_thread_count(n, config);

    if (metrics) {
        metrics->threads_used = T;
        metrics->rows_examined = n;
    }

    // Fast-path sequential
    if (T <= 1) {
        std::vector<Row> result;
        result.reserve(n);
        for (const auto& row : rows) {
            if (!predicate || eval_predicate(predicate, schema, row)) {
                if (projection_indices.empty()) {
                    result.push_back(row);
                } else {
                    Row proj;
                    proj.reserve(projection_indices.size());
                    for (int idx : projection_indices) proj.push_back(row[idx]);
                    result.push_back(std::move(proj));
                }
            }
        }
        if (metrics) metrics->rows_returned = result.size();
        return result;
    }

    // Parallel multi-worker execution
    std::vector<std::vector<Row>> worker_results(T);
    std::vector<std::exception_ptr> errors(T, nullptr);
    std::vector<std::thread> workers;
    workers.reserve(T);

    size_t chunk_size = (n + T - 1) / T;

    for (size_t t = 0; t < T; ++t) {
        size_t start = t * chunk_size;
        size_t end = std::min(start + chunk_size, n);

        workers.emplace_back([&, t, start, end]() {
            try {
                std::vector<Row> local_rows;
                if (start < end) {
                    local_rows.reserve(end - start);
                    for (size_t r = start; r < end; ++r) {
                        const auto& row = rows[r];
                        if (!predicate || eval_predicate(predicate, schema, row)) {
                            if (projection_indices.empty()) {
                                local_rows.push_back(row);
                            } else {
                                Row proj;
                                proj.reserve(projection_indices.size());
                                for (int idx : projection_indices) proj.push_back(row[idx]);
                                local_rows.push_back(std::move(proj));
                            }
                        }
                    }
                }
                worker_results[t] = std::move(local_rows);
            } catch (...) {
                errors[t] = std::current_exception();
            }
        });
    }

    // Join workers
    for (auto& w : workers) {
        if (w.joinable()) w.join();
    }

    // Check for exceptions
    for (size_t t = 0; t < T; ++t) {
        if (errors[t]) std::rethrow_exception(errors[t]);
    }

    // Combine results
    size_t total_returned = 0;
    for (const auto& res : worker_results) total_returned += res.size();

    std::vector<Row> combined;
    combined.reserve(total_returned);
    for (auto& res : worker_results) {
        for (auto& r : res) combined.push_back(std::move(r));
    }

    if (metrics) metrics->rows_returned = combined.size();
    return combined;
}

// ═══════════════════════════════════════════════════════════════════════
//  PARALLEL AGGREGATION
// ═══════════════════════════════════════════════════════════════════════

struct WorkerAggState {
    AggFunc func;
    int64_t count = 0;
    double  sum   = 0.0;
    Value   min_val = std::monostate{};
    Value   max_val = std::monostate{};

    void accumulate(const Value& v) {
        bool is_null = std::holds_alternative<std::monostate>(v);
        if (func == AggFunc::Count) {
            ++count;
            return;
        }
        if (is_null) return;
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

    void merge(const WorkerAggState& other) {
        count += other.count;
        sum   += other.sum;
        if (!std::holds_alternative<std::monostate>(other.min_val)) {
            if (std::holds_alternative<std::monostate>(min_val) || compare_values(other.min_val, min_val) < 0) {
                min_val = other.min_val;
            }
        }
        if (!std::holds_alternative<std::monostate>(other.max_val)) {
            if (std::holds_alternative<std::monostate>(max_val) || compare_values(other.max_val, max_val) > 0) {
                max_val = other.max_val;
            }
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

static std::string make_group_key(const std::vector<Value>& vals) {
    std::string key;
    for (size_t i = 0; i < vals.size(); ++i) {
        if (i > 0) key += '\x01';
        key += value_to_sort_string(vals[i]);
    }
    return key;
}

Table ParallelEngine::parallel_aggregate(
    const std::vector<ColumnDef>& work_schema,
    const std::vector<Row>& filtered_rows,
    const std::vector<ExprPtr>& group_by,
    const std::vector<SelectItem>& select_list,
    QueryMetrics* metrics,
    ParallelConfig config) {

    size_t n = filtered_rows.size();
    size_t T = resolve_thread_count(n, config);

    if (metrics) {
        metrics->threads_used = T;
        metrics->rows_examined = n;
    }

    // Resolve group by column indices
    std::vector<int> group_col_indices;
    for (const auto& gb : group_by) {
        auto* col = dynamic_cast<ColumnRef*>(gb.get());
        if (!col) throw SqlError("GROUP BY requires column references");
        int idx = resolve_column(*col, work_schema);
        if (idx < 0) throw SqlError("unknown column in GROUP BY: " + col->column);
        group_col_indices.push_back(idx);
    }

    // Result schema
    std::vector<ColumnDef> result_schema;
    for (const auto& item : select_list) {
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

    // Structure for storing worker partial aggregation
    struct GroupAgg {
        std::vector<Value> group_vals;
        std::vector<WorkerAggState> agg_states;
    };

    auto compute_slice = [&](size_t start, size_t end) -> std::map<std::string, GroupAgg> {
        std::map<std::string, GroupAgg> local_groups;

        for (size_t r = start; r < end; ++r) {
            const auto& row = filtered_rows[r];
            std::vector<Value> key_vals;
            for (int idx : group_col_indices) key_vals.push_back(row[idx]);
            std::string key = make_group_key(key_vals);

            auto it = local_groups.find(key);
            if (it == local_groups.end()) {
                GroupAgg ga;
                ga.group_vals = key_vals;
                for (const auto& item : select_list) {
                    if (auto* agg = dynamic_cast<AggExpr*>(item.expr.get())) {
                        WorkerAggState st;
                        st.func = agg->func;
                        ga.agg_states.push_back(st);
                    }
                }
                it = local_groups.emplace(key, std::move(ga)).first;
            }

            size_t agg_idx = 0;
            for (const auto& item : select_list) {
                if (auto* agg = dynamic_cast<AggExpr*>(item.expr.get())) {
                    if (dynamic_cast<StarExpr*>(agg->arg.get())) {
                        it->second.agg_states[agg_idx].count++;
                    } else if (auto* arg_col = dynamic_cast<ColumnRef*>(agg->arg.get())) {
                        int idx = resolve_column(*arg_col, work_schema);
                        it->second.agg_states[agg_idx].accumulate(row[idx]);
                    }
                    ++agg_idx;
                }
            }
        }

        return local_groups;
    };

    std::map<std::string, GroupAgg> global_groups;

    if (T <= 1) {
        global_groups = compute_slice(0, n);
    } else {
        std::vector<std::map<std::string, GroupAgg>> worker_maps(T);
        std::vector<std::exception_ptr> errors(T, nullptr);
        std::vector<std::thread> workers;
        size_t chunk_size = (n + T - 1) / T;

        for (size_t t = 0; t < T; ++t) {
            size_t start = t * chunk_size;
            size_t end = std::min(start + chunk_size, n);

            workers.emplace_back([&, t, start, end]() {
                try {
                    worker_maps[t] = compute_slice(start, end);
                } catch (...) {
                    errors[t] = std::current_exception();
                }
            });
        }

        for (auto& w : workers) {
            if (w.joinable()) w.join();
        }

        for (size_t t = 0; t < T; ++t) {
            if (errors[t]) std::rethrow_exception(errors[t]);
        }

        // Merge worker maps
        for (const auto& w_map : worker_maps) {
            for (const auto& [key, ga] : w_map) {
                auto it = global_groups.find(key);
                if (it == global_groups.end()) {
                    global_groups[key] = ga;
                } else {
                    for (size_t a = 0; a < ga.agg_states.size(); ++a) {
                        it->second.agg_states[a].merge(ga.agg_states[a]);
                    }
                }
            }
        }
    }

    // Handle empty table with aggregation
    if (group_col_indices.empty() && global_groups.empty()) {
        GroupAgg empty_ga;
        for (const auto& item : select_list) {
            if (auto* agg = dynamic_cast<AggExpr*>(item.expr.get())) {
                WorkerAggState st;
                st.func = agg->func;
                empty_ga.agg_states.push_back(st);
            }
        }
        global_groups[""] = std::move(empty_ga);
    }

    // Build result rows
    std::vector<Row> result_rows;
    result_rows.reserve(global_groups.size());

    for (const auto& [key, ga] : global_groups) {
        Row row;
        size_t agg_idx = 0;
        size_t grp_idx = 0;

        for (const auto& item : select_list) {
            if (dynamic_cast<AggExpr*>(item.expr.get())) {
                row.push_back(ga.agg_states[agg_idx++].result());
            } else if (dynamic_cast<ColumnRef*>(item.expr.get())) {
                if (grp_idx < ga.group_vals.size()) {
                    row.push_back(ga.group_vals[grp_idx++]);
                } else {
                    row.push_back(std::monostate{});
                }
            }
        }
        result_rows.push_back(std::move(row));
    }

    if (metrics) metrics->rows_returned = result_rows.size();
    return Table("agg_result", std::move(result_schema), std::move(result_rows));
}

}  // namespace snowquery
