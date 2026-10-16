/// @file test_parallel.cpp
/// @brief Automated tests for Parallel Scans, Parallel Aggregation, and VectorBatch.

#include "snowquery/parallel.hpp"
#include "snowquery/table.hpp"

#include <iostream>
#include <string>

static int g_tests_run    = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define ASSERT_TRUE(expr)                                                      \
    do {                                                                        \
        ++g_tests_run;                                                         \
        if ((expr)) {                                                          \
            ++g_tests_passed;                                                  \
        } else {                                                               \
            ++g_tests_failed;                                                  \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__                \
                      << "  " #expr " is false\n";                             \
        }                                                                      \
    } while (false)

#define ASSERT_EQ(a, b)                                                       \
    do {                                                                        \
        ++g_tests_run;                                                         \
        if ((a) == (b)) {                                                      \
            ++g_tests_passed;                                                  \
        } else {                                                               \
            ++g_tests_failed;                                                  \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__                \
                      << "  " #a " != " #b << "\n";                           \
        }                                                                      \
    } while (false)

void test_vector_batch() {
    std::vector<snowquery::ColumnDef> schema = {
        {"id", snowquery::DataType::Integer},
        {"val", snowquery::DataType::Float}
    };
    std::vector<snowquery::Row> rows = {
        {int64_t{10}, 1.5},
        {int64_t{20}, 2.5},
        {int64_t{30}, 3.5}
    };

    auto vb = snowquery::VectorBatch::from_rows(schema, rows, 0, 3);
    ASSERT_EQ(vb.size, 3u);
    ASSERT_EQ(vb.columns.size(), 2u);

    auto roundtrip_rows = vb.to_rows();
    ASSERT_EQ(roundtrip_rows.size(), 3u);
    ASSERT_EQ(std::get<int64_t>(roundtrip_rows[1][0]), 20);
    ASSERT_EQ(std::get<double>(roundtrip_rows[1][1]), 2.5);
}

void test_parallel_scan_equivalence() {
    std::vector<snowquery::ColumnDef> schema = {
        {"id", snowquery::DataType::Integer},
        {"category", snowquery::DataType::Text},
        {"score", snowquery::DataType::Float}
    };

    // 5000 rows
    std::vector<snowquery::Row> rows;
    rows.reserve(5000);
    for (int64_t i = 0; i < 5000; ++i) {
        std::string cat = (i % 2 == 0) ? "EVEN" : "ODD";
        rows.push_back({i, cat, static_cast<double>(i) * 1.5});
    }

    // Filter: score > 3000.0 (i > 2000 -> 3000 rows)
    auto pred = std::make_shared<snowquery::BinaryExpr>();
    pred->op = snowquery::BinaryExpr::Gt;
    auto col_ref = std::make_shared<snowquery::ColumnRef>();
    col_ref->column = "score";
    auto lit = std::make_shared<snowquery::FloatLit>();
    lit->value = 3000.0;
    pred->left = col_ref;
    pred->right = lit;

    // Sequential run (config with 1 thread)
    snowquery::ParallelConfig seq_cfg;
    seq_cfg.num_threads = 1;
    snowquery::QueryMetrics seq_metrics;
    auto seq_results = snowquery::ParallelEngine::parallel_scan_filter(
        schema, rows, pred, {}, &seq_metrics, seq_cfg);

    // Parallel run (auto-detect threads)
    snowquery::ParallelConfig par_cfg;
    par_cfg.num_threads = 4;
    snowquery::QueryMetrics par_metrics;
    auto par_results = snowquery::ParallelEngine::parallel_scan_filter(
        schema, rows, pred, {}, &par_metrics, par_cfg);

    ASSERT_EQ(seq_results.size(), par_results.size());
    ASSERT_EQ(par_results.size(), 2999u);  // 2001 .. 4999 inclusive
    ASSERT_EQ(par_metrics.threads_used, 4u);
    ASSERT_EQ(seq_metrics.threads_used, 1u);

    // Verify row values match between sequential and parallel
    for (size_t i = 0; i < seq_results.size(); ++i) {
        ASSERT_EQ(std::get<int64_t>(seq_results[i][0]), std::get<int64_t>(par_results[i][0]));
    }
}

void test_parallel_aggregate_equivalence() {
    std::vector<snowquery::ColumnDef> schema = {
        {"category", snowquery::DataType::Text},
        {"amount", snowquery::DataType::Float}
    };

    std::vector<snowquery::Row> rows;
    rows.reserve(4000);
    const char* cats[] = {"North", "South", "East", "West"};
    for (size_t i = 0; i < 4000; ++i) {
        rows.push_back({std::string(cats[i % 4]), static_cast<double>(i + 1)});
    }

    // SELECT category, COUNT(*), SUM(amount), MIN(amount), MAX(amount) GROUP BY category
    auto col_cat = std::make_shared<snowquery::ColumnRef>();
    col_cat->column = "category";

    auto count_star = std::make_shared<snowquery::AggExpr>();
    count_star->func = snowquery::AggFunc::Count;
    count_star->arg = std::make_shared<snowquery::StarExpr>();

    auto sum_amt = std::make_shared<snowquery::AggExpr>();
    sum_amt->func = snowquery::AggFunc::Sum;
    auto col_amt = std::make_shared<snowquery::ColumnRef>();
    col_amt->column = "amount";
    sum_amt->arg = col_amt;

    std::vector<snowquery::SelectItem> select_list = {
        {col_cat, "category"},
        {count_star, "count"},
        {sum_amt, "total"}
    };

    snowquery::ParallelConfig par_cfg;
    par_cfg.num_threads = 4;
    snowquery::QueryMetrics par_metrics;
    snowquery::Table par_agg = snowquery::ParallelEngine::parallel_aggregate(
        schema, rows, {col_cat}, select_list, &par_metrics, par_cfg);

    ASSERT_EQ(par_agg.row_count(), 4u);
    ASSERT_EQ(par_metrics.threads_used, 4u);

    // Each category should have 1000 items
    for (const auto& r : par_agg.rows()) {
        ASSERT_EQ(std::get<int64_t>(r[1]), 1000);
    }
}

int main() {
    std::cout << "Running Parallel Engine Tests...\n";
    test_vector_batch();
    test_parallel_scan_equivalence();
    test_parallel_aggregate_equivalence();

    std::cout << "Results: " << g_tests_passed << "/" << g_tests_run << " passed";
    if (g_tests_failed > 0) {
        std::cout << " (" << g_tests_failed << " FAILED)";
    }
    std::cout << "\n";

    return (g_tests_failed == 0) ? 0 : 1;
}
