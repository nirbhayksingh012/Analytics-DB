/// @file test_optimizer.cpp
/// @brief Automated tests for Logical Planning, Predicate Pushdown, Projection Pushdown, and EXPLAIN.

#include "snowquery/optimizer.hpp"
#include "snowquery/sql_parser.hpp"
#include "snowquery/executor.hpp"

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

void test_logical_plan_generation() {
    auto stmt = snowquery::parse_sql(
        "SELECT id, name FROM users WHERE id > 10 ORDER BY name ASC LIMIT 5;");
    auto plan = snowquery::QueryOptimizer::build_logical_plan(stmt);

    ASSERT_TRUE(plan != nullptr);
    ASSERT_EQ(plan->type(), snowquery::LogicalNodeType::Limit);

    auto* limit_node = dynamic_cast<snowquery::LogicalLimit*>(plan.get());
    ASSERT_TRUE(limit_node != nullptr);
    ASSERT_EQ(limit_node->limit_val, 5);

    auto* sort_node = dynamic_cast<snowquery::LogicalSort*>(limit_node->child.get());
    ASSERT_TRUE(sort_node != nullptr);

    auto* proj_node = dynamic_cast<snowquery::LogicalProject*>(sort_node->child.get());
    ASSERT_TRUE(proj_node != nullptr);

    auto* filter_node = dynamic_cast<snowquery::LogicalFilter*>(proj_node->child.get());
    ASSERT_TRUE(filter_node != nullptr);

    auto* scan_node = dynamic_cast<snowquery::LogicalScan*>(filter_node->child.get());
    ASSERT_TRUE(scan_node != nullptr);
    ASSERT_EQ(scan_node->table_name, "users");
}

void test_predicate_and_projection_pushdown() {
    auto stmt = snowquery::parse_sql(
        "SELECT name, salary FROM employees WHERE salary >= 80000;");
    auto plan = snowquery::QueryOptimizer::build_logical_plan(stmt);
    auto opt_plan = snowquery::QueryOptimizer::optimize(plan);

    ASSERT_TRUE(opt_plan != nullptr);

    // After optimization, scan should have pushed predicate and projected columns
    auto* proj_node = dynamic_cast<snowquery::LogicalProject*>(opt_plan.get());
    ASSERT_TRUE(proj_node != nullptr);

    // LogicalFilter pushed directly to scan, so child of Project is LogicalScan!
    auto* scan_node = dynamic_cast<snowquery::LogicalScan*>(proj_node->child.get());
    ASSERT_TRUE(scan_node != nullptr);

    // Predicate pushdown verified
    ASSERT_EQ(scan_node->pushed_predicates.size(), 1u);

    // Projection pushdown (column pruning): only "name" and "salary" needed
    ASSERT_EQ(scan_node->projected_columns.size(), 2u);
    ASSERT_EQ(scan_node->projected_columns[0], "name");
    ASSERT_EQ(scan_node->projected_columns[1], "salary");
}

void test_explain_output() {
    auto stmt = snowquery::parse_sql(
        "EXPLAIN SELECT city, COUNT(*), SUM(amount) FROM orders WHERE amount > 100 GROUP BY city;");
    ASSERT_TRUE(stmt.is_explain);

    snowquery::Table orders_tbl("orders", {
        {"city", snowquery::DataType::Text},
        {"amount", snowquery::DataType::Float}
    });
    snowquery::Catalog catalog;
    catalog.add_table("orders", &orders_tbl);

    auto res = snowquery::execute_stmt(stmt, catalog);
    ASSERT_TRUE(!res.explain_text.empty());

    // Verify key explain annotations
    ASSERT_TRUE(res.explain_text.find("LogicalScan: orders") != std::string::npos);
    ASSERT_TRUE(res.explain_text.find("predicate pushdown: APPLIED") != std::string::npos);
    ASSERT_TRUE(res.explain_text.find("projection pushdown: APPLIED") != std::string::npos);
    ASSERT_TRUE(res.explain_text.find("Min/Max block pruning ENABLED") != std::string::npos);
}

int main() {
    std::cout << "Running Query Optimizer Tests...\n";
    test_logical_plan_generation();
    test_predicate_and_projection_pushdown();
    test_explain_output();

    std::cout << "Results: " << g_tests_passed << "/" << g_tests_run << " passed";
    if (g_tests_failed > 0) {
        std::cout << " (" << g_tests_failed << " FAILED)";
    }
    std::cout << "\n";

    return (g_tests_failed == 0) ? 0 : 1;
}
