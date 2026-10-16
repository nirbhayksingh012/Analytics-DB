/// @file benchmark.cpp
/// @brief Reproducible benchmark suite for SnowQuery analytical engine.

#include "snowquery/column_store.hpp"
#include "snowquery/executor.hpp"
#include "snowquery/metrics.hpp"
#include "snowquery/optimizer.hpp"
#include "snowquery/parallel.hpp"
#include "snowquery/sql_parser.hpp"
#include "snowquery/storage.hpp"
#include "snowquery/table.hpp"

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace snowquery;

static Table generate_orders(size_t num_rows) {
    std::vector<ColumnDef> schema = {
        {"order_id", DataType::Integer},
        {"customer_id", DataType::Integer},
        {"city", DataType::Text},
        {"product", DataType::Text},
        {"quantity", DataType::Integer},
        {"amount", DataType::Float}
    };

    const std::vector<std::string> cities = {
        "New York", "San Francisco", "Chicago", "Seattle", "Austin", "Boston"
    };
    const std::vector<std::string> products = {
        "Laptop", "Phone", "Tablet", "Monitor", "Keyboard", "Headphones"
    };

    std::vector<Row> rows;
    rows.reserve(num_rows);

    for (size_t i = 0; i < num_rows; ++i) {
        int64_t oid = static_cast<int64_t>(1000000 + i);
        int64_t cid = static_cast<int64_t>(100 + (i % 1000));
        std::string city = cities[i % cities.size()];
        std::string prod = products[i % products.size()];
        int64_t qty = static_cast<int64_t>(1 + (i % 5));
        double amt = 25.0 + static_cast<double>(i % 500) * 3.5;

        rows.push_back({oid, cid, std::move(city), std::move(prod), qty, amt});
    }

    return Table("orders", std::move(schema), std::move(rows));
}

static Table generate_customers(size_t num_customers) {
    std::vector<ColumnDef> schema = {
        {"id", DataType::Integer},
        {"name", DataType::Text},
        {"tier", DataType::Text}
    };
    const std::vector<std::string> tiers = {"Bronze", "Silver", "Gold", "Platinum"};

    std::vector<Row> rows;
    rows.reserve(num_customers);

    for (size_t i = 0; i < num_customers; ++i) {
        int64_t id = static_cast<int64_t>(100 + i);
        std::string name = "Customer_" + std::to_string(id);
        std::string tier = tiers[i % tiers.size()];
        rows.push_back({id, std::move(name), std::move(tier)});
    }

    return Table("customers", std::move(schema), std::move(rows));
}

struct BenchResult {
    std::string name;
    size_t      rows;
    double      time_ms;
    size_t      rows_examined;
    size_t      rows_returned;
    size_t      blocks_examined;
    size_t      blocks_skipped;
    size_t      threads_used;
};

static void print_bench_header() {
    std::cout << "\n| Benchmark Operation | Dataset Rows | Time (ms) | Rows Examined | Returned | Blocks Skip | Threads |\n";
    std::cout << "|---------------------|--------------|-----------|---------------|----------|-------------|---------|\n";
}

static void print_bench_row(const BenchResult& r) {
    std::cout << "| " << std::left << std::setw(19) << r.name << " | "
              << std::right << std::setw(12) << r.rows << " | "
              << std::fixed << std::setprecision(2) << std::setw(9) << r.time_ms << " | "
              << std::setw(13) << r.rows_examined << " | "
              << std::setw(8) << r.rows_returned << " | "
              << std::setw(11) << r.blocks_skipped << " | "
              << std::setw(7) << r.threads_used << " |\n";
}

int main() {
    std::cout << "====================================================================\n";
    std::cout << "          SnowQuery High-Performance Engine Benchmarks              \n";
    std::cout << "====================================================================\n";

    const size_t ROWS_SMALL = 10000;
    const size_t ROWS_LARGE = 100000;

    std::cout << "\n[1/7] Generating synthetic datasets (" << ROWS_SMALL << " and " << ROWS_LARGE << " rows)...\n";
    auto orders_10k = generate_orders(ROWS_SMALL);
    auto orders_100k = generate_orders(ROWS_LARGE);
    auto customers = generate_customers(1000);

    Catalog cat_10k;
    cat_10k.add_table("orders", &orders_10k);
    cat_10k.add_table("customers", &customers);

    Catalog cat_100k;
    cat_100k.add_table("orders", &orders_100k);
    cat_100k.add_table("customers", &customers);

    print_bench_header();

    // ── 1. Full Table Scan ──────────────────────────────────────────
    {
        auto res = execute_query("SELECT * FROM orders;", cat_10k);
        print_bench_row({"Scan (10k)", ROWS_SMALL, res.metrics.total_time_ms(),
                         res.metrics.rows_examined, res.metrics.rows_returned,
                         res.metrics.blocks_examined, res.metrics.blocks_skipped, res.metrics.threads_used});
    }
    {
        auto res = execute_query("SELECT * FROM orders;", cat_100k);
        print_bench_row({"Scan (100k)", ROWS_LARGE, res.metrics.total_time_ms(),
                         res.metrics.rows_examined, res.metrics.rows_returned,
                         res.metrics.blocks_examined, res.metrics.blocks_skipped, res.metrics.threads_used});
    }

    // ── 2. Selective Filter (WHERE amount > 1500) ───────────────────
    {
        auto res = execute_query("SELECT order_id, amount FROM orders WHERE amount > 1500.0;", cat_100k);
        print_bench_row({"Filter Selective", ROWS_LARGE, res.metrics.total_time_ms(),
                         res.metrics.rows_examined, res.metrics.rows_returned,
                         res.metrics.blocks_examined, res.metrics.blocks_skipped, res.metrics.threads_used});
    }

    // ── 3. GROUP BY & Aggregation ───────────────────────────────────
    {
        auto res = execute_query(
            "SELECT city, COUNT(*), SUM(amount), MIN(amount), MAX(amount) "
            "FROM orders GROUP BY city ORDER BY SUM(amount) DESC;", cat_100k);
        print_bench_row({"Group By Aggs", ROWS_LARGE, res.metrics.total_time_ms(),
                         res.metrics.rows_examined, res.metrics.rows_returned,
                         res.metrics.blocks_examined, res.metrics.blocks_skipped, res.metrics.threads_used});
    }

    // ── 4. Hash Join ────────────────────────────────────────────────
    {
        auto res = execute_query(
            "SELECT orders.order_id, customers.name, orders.amount "
            "FROM orders JOIN customers ON orders.customer_id = customers.id "
            "WHERE orders.amount > 1000.0;", cat_100k);
        print_bench_row({"Hash Join (100k)", ROWS_LARGE, res.metrics.total_time_ms(),
                         res.metrics.rows_examined, res.metrics.rows_returned,
                         res.metrics.blocks_examined, res.metrics.blocks_skipped, res.metrics.threads_used});
    }

    // ── 5. Persistent Columnar Storage Roundtrip ────────────────────
    std::string scol_path = "bench_orders.scol";
    {
        auto t0 = std::chrono::high_resolution_clock::now();
        ColumnarStorage::write_file(scol_path, orders_100k, 1024);
        auto t1 = std::chrono::high_resolution_clock::now();
        double write_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        print_bench_row({"Storage Write", ROWS_LARGE, write_ms, ROWS_LARGE, ROWS_LARGE, 0, 0, 1});

        t0 = std::chrono::high_resolution_clock::now();
        Table loaded = ColumnarStorage::read_file(scol_path);
        t1 = std::chrono::high_resolution_clock::now();
        double read_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        print_bench_row({"Storage Read", ROWS_LARGE, read_ms, ROWS_LARGE, loaded.row_count(), 0, 0, 1});
    }

    // ── 6. Block Pruning (Data Skipping) ────────────────────────────
    {
        // Query matching top ~5% of orders: order_id >= 1095000
        auto pred = std::make_shared<BinaryExpr>();
        pred->op = BinaryExpr::Ge;
        auto col = std::make_shared<ColumnRef>();
        col->column = "order_id";
        auto lit = std::make_shared<IntLit>();
        lit->value = 1095000;
        pred->left = col;
        pred->right = lit;

        QueryMetrics pruning_metrics;
        auto t0 = std::chrono::high_resolution_clock::now();
        Table filtered = ColumnarStorage::read_file_filtered(scol_path, {"order_id", "amount"}, pred, &pruning_metrics);
        auto t1 = std::chrono::high_resolution_clock::now();
        double prune_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        print_bench_row({"Block Pruning ON", ROWS_LARGE, prune_ms,
                         pruning_metrics.rows_examined, filtered.row_count(),
                         pruning_metrics.blocks_examined, pruning_metrics.blocks_skipped, 1});
    }

    // ── 7. Sequential vs Parallel Scan ──────────────────────────────
    {
        auto pred = std::make_shared<BinaryExpr>();
        pred->op = BinaryExpr::Gt;
        auto col = std::make_shared<ColumnRef>();
        col->column = "amount";
        auto lit = std::make_shared<FloatLit>();
        lit->value = 500.0;
        pred->left = col;
        pred->right = lit;

        ParallelConfig seq_cfg;
        seq_cfg.num_threads = 1;
        QueryMetrics seq_m;
        auto t0 = std::chrono::high_resolution_clock::now();
        auto seq_res = ParallelEngine::parallel_scan_filter(
            orders_100k.schema(), orders_100k.rows(), pred, {}, &seq_m, seq_cfg);
        auto t1 = std::chrono::high_resolution_clock::now();
        double seq_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        print_bench_row({"Scan Sequential", ROWS_LARGE, seq_ms,
                         seq_m.rows_examined, seq_res.size(), 0, 0, 1});

        ParallelConfig par_cfg;
        par_cfg.num_threads = 4;
        QueryMetrics par_m;
        t0 = std::chrono::high_resolution_clock::now();
        auto par_res = ParallelEngine::parallel_scan_filter(
            orders_100k.schema(), orders_100k.rows(), pred, {}, &par_m, par_cfg);
        t1 = std::chrono::high_resolution_clock::now();
        double par_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        print_bench_row({"Scan Parallel 4T", ROWS_LARGE, par_ms,
                         par_m.rows_examined, par_res.size(), 0, 0, par_m.threads_used});

        std::cout << "\nParallel Speedup Factor: " << std::fixed << std::setprecision(2)
                  << (seq_ms / par_ms) << "x\n";
    }

    std::filesystem::remove(scol_path);
    std::cout << "====================================================================\n";
    std::cout << "                      Benchmark Complete                            \n";
    std::cout << "====================================================================\n";

    return 0;
}
