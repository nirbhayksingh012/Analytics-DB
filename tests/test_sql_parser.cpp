/// @file test_sql_parser.cpp
/// @brief Automated tests for SQL tokenizer, parser, and AST creation.

#include "snowquery/sql_parser.hpp"
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

#define ASSERT_THROWS(expr, ExceptionType)                                     \
    do {                                                                        \
        ++g_tests_run;                                                         \
        bool caught = false;                                                   \
        try {                                                                  \
            expr;                                                              \
        } catch (const ExceptionType&) {                                       \
            caught = true;                                                     \
        } catch (...) {}                                                       \
        if (caught) {                                                          \
            ++g_tests_passed;                                                  \
        } else {                                                               \
            ++g_tests_failed;                                                  \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__                \
                      << "  expected " #ExceptionType " not thrown\n";         \
        }                                                                      \
    } while (false)

void test_basic_select() {
    auto stmt = snowquery::parse_sql("SELECT id, name FROM users;");
    ASSERT_EQ(stmt.from_table, "users");
    ASSERT_EQ(stmt.select_list.size(), 2u);
    ASSERT_TRUE(stmt.where_clause == nullptr);
    ASSERT_EQ(stmt.order_by.size(), 0u);
    ASSERT_TRUE(!stmt.limit.has_value());
}

void test_select_star() {
    auto stmt = snowquery::parse_sql("SELECT * FROM orders");
    ASSERT_EQ(stmt.from_table, "orders");
    ASSERT_EQ(stmt.select_list.size(), 1u);
    ASSERT_TRUE(dynamic_cast<snowquery::StarExpr*>(stmt.select_list[0].expr.get()) != nullptr);
}

void test_where_comparisons_and_precedence() {
    auto stmt = snowquery::parse_sql(
        "SELECT id FROM orders WHERE amount >= 100 AND city = 'New York' OR status != 'CANCELLED';");
    ASSERT_TRUE(stmt.where_clause != nullptr);

    // OR should be top level of binary expression
    auto* top = dynamic_cast<snowquery::BinaryExpr*>(stmt.where_clause.get());
    ASSERT_TRUE(top != nullptr);
    ASSERT_TRUE(top->op == snowquery::BinaryExpr::Or);

    // Left side of OR should be AND expression
    auto* left_and = dynamic_cast<snowquery::BinaryExpr*>(top->left.get());
    ASSERT_TRUE(left_and != nullptr);
    ASSERT_TRUE(left_and->op == snowquery::BinaryExpr::And);
}

void test_parentheses_precedence() {
    auto stmt = snowquery::parse_sql(
        "SELECT id FROM orders WHERE (amount > 100 OR city = 'NY') AND status = 'OK';");
    auto* top = dynamic_cast<snowquery::BinaryExpr*>(stmt.where_clause.get());
    ASSERT_TRUE(top != nullptr);
    ASSERT_TRUE(top->op == snowquery::BinaryExpr::And);

    auto* left_or = dynamic_cast<snowquery::BinaryExpr*>(top->left.get());
    ASSERT_TRUE(left_or != nullptr);
    ASSERT_TRUE(left_or->op == snowquery::BinaryExpr::Or);
}

void test_aggregates_and_group_by() {
    auto stmt = snowquery::parse_sql(
        "SELECT city, COUNT(*), SUM(amount), MIN(amount), MAX(amount) "
        "FROM orders GROUP BY city ORDER BY SUM(amount) DESC LIMIT 5;");
    ASSERT_EQ(stmt.from_table, "orders");
    ASSERT_EQ(stmt.select_list.size(), 5u);
    ASSERT_EQ(stmt.group_by.size(), 1u);
    ASSERT_EQ(stmt.order_by.size(), 1u);
    ASSERT_TRUE(stmt.order_by[0].dir == snowquery::SortDir::Desc);
    ASSERT_TRUE(stmt.limit.has_value());
    ASSERT_EQ(*stmt.limit, 5);
}

void test_join_syntax() {
    auto stmt = snowquery::parse_sql(
        "SELECT users.name, orders.amount "
        "FROM users JOIN orders ON users.id = orders.user_id;");
    ASSERT_EQ(stmt.from_table, "users");
    ASSERT_EQ(stmt.joins.size(), 1u);
    ASSERT_EQ(stmt.joins[0].table_name, "orders");
    ASSERT_TRUE(stmt.joins[0].on_condition != nullptr);
}

void test_explain_syntax() {
    auto stmt = snowquery::parse_sql("EXPLAIN SELECT a FROM t WHERE b = 1;");
    ASSERT_TRUE(stmt.is_explain);
    ASSERT_EQ(stmt.from_table, "t");
}

void test_syntax_errors() {
    // Missing FROM
    ASSERT_THROWS(snowquery::parse_sql("SELECT a, b;"), snowquery::SqlError);
    // Unmatched paren
    ASSERT_THROWS(snowquery::parse_sql("SELECT a FROM t WHERE (b = 1;"), snowquery::SqlError);
    // Missing table name
    ASSERT_THROWS(snowquery::parse_sql("SELECT a FROM ;"), snowquery::SqlError);
    // Bad token
    ASSERT_THROWS(snowquery::parse_sql("SELECT a FROM t WHERE #$%;"), snowquery::SqlError);
}

int main() {
    std::cout << "Running SQL Parser Tests...\n";
    test_basic_select();
    test_select_star();
    test_where_comparisons_and_precedence();
    test_parentheses_precedence();
    test_aggregates_and_group_by();
    test_join_syntax();
    test_explain_syntax();
    test_syntax_errors();

    std::cout << "Results: " << g_tests_passed << "/" << g_tests_run << " passed";
    if (g_tests_failed > 0) {
        std::cout << " (" << g_tests_failed << " FAILED)";
    }
    std::cout << "\n";

    return (g_tests_failed == 0) ? 0 : 1;
}
