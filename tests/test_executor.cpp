/// @file test_executor.cpp
/// @brief Automated tests for SQL Executor (filtering, grouping, sorting, aggregates, joins).

#include "snowquery/executor.hpp"
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

static snowquery::Table create_employees_table() {
    std::vector<snowquery::ColumnDef> schema = {
        {"id", snowquery::DataType::Integer},
        {"name", snowquery::DataType::Text},
        {"dept", snowquery::DataType::Text},
        {"salary", snowquery::DataType::Float}
    };
    std::vector<snowquery::Row> rows = {
        {int64_t{1}, std::string("Alice"), std::string("Engineering"), 120000.0},
        {int64_t{2}, std::string("Bob"),   std::string("Sales"),        80000.0},
        {int64_t{3}, std::string("Charlie"), std::string("Engineering"), 110000.0},
        {int64_t{4}, std::string("Diana"), std::string("Marketing"),    95000.0},
        {int64_t{5}, std::string("Evan"),  std::string("Sales"),        85000.0}
    };
    return snowquery::Table("employees", schema, rows);
}

static snowquery::Table create_departments_table() {
    std::vector<snowquery::ColumnDef> schema = {
        {"dept_name", snowquery::DataType::Text},
        {"floor", snowquery::DataType::Integer}
    };
    std::vector<snowquery::Row> rows = {
        {std::string("Engineering"), int64_t{3}},
        {std::string("Sales"), int64_t{1}},
        {std::string("Marketing"), int64_t{2}}
    };
    return snowquery::Table("departments", schema, rows);
}

void test_simple_select_filter() {
    auto emp = create_employees_table();
    snowquery::Catalog cat;
    cat.add_table("employees", &emp);

    auto res = snowquery::execute_query(
        "SELECT name, salary FROM employees WHERE salary > 90000 ORDER BY salary DESC;", cat);
    ASSERT_EQ(res.result.row_count(), 3u);
    ASSERT_EQ(res.result.col_count(), 2u);

    // First row should be Alice
    auto& r0 = res.result.rows()[0];
    ASSERT_EQ(std::get<std::string>(r0[0]), "Alice");
    ASSERT_EQ(std::get<double>(r0[1]), 120000.0);
}

void test_where_boolean_logic() {
    auto emp = create_employees_table();
    snowquery::Catalog cat;
    cat.add_table("employees", &emp);

    auto res = snowquery::execute_query(
        "SELECT name FROM employees WHERE dept = 'Sales' OR (dept = 'Engineering' AND salary > 115000);", cat);
    ASSERT_EQ(res.result.row_count(), 3u); // Bob, Evan, Alice
}

void test_limit() {
    auto emp = create_employees_table();
    snowquery::Catalog cat;
    cat.add_table("employees", &emp);

    auto res = snowquery::execute_query("SELECT name FROM employees LIMIT 2;", cat);
    ASSERT_EQ(res.result.row_count(), 2u);
}

void test_group_by_aggregates() {
    auto emp = create_employees_table();
    snowquery::Catalog cat;
    cat.add_table("employees", &emp);

    auto res = snowquery::execute_query(
        "SELECT dept, COUNT(*), SUM(salary), MIN(salary), MAX(salary) "
        "FROM employees GROUP BY dept ORDER BY dept ASC;", cat);

    ASSERT_EQ(res.result.row_count(), 3u); // Engineering, Marketing, Sales
    auto& r_eng = res.result.rows()[0];
    ASSERT_EQ(std::get<std::string>(r_eng[0]), "Engineering");
    ASSERT_EQ(std::get<int64_t>(r_eng[1]), 2); // 2 employees
    ASSERT_EQ(std::get<double>(r_eng[2]), 230000.0); // 120k + 110k
    ASSERT_EQ(std::get<double>(r_eng[3]), 110000.0);
    ASSERT_EQ(std::get<double>(r_eng[4]), 120000.0);
}

void test_hash_join() {
    auto emp = create_employees_table();
    auto dept = create_departments_table();
    snowquery::Catalog cat;
    cat.add_table("employees", &emp);
    cat.add_table("departments", &dept);

    auto res = snowquery::execute_query(
        "SELECT employees.name, departments.floor "
        "FROM employees JOIN departments ON employees.dept = departments.dept_name "
        "WHERE departments.floor >= 2 ORDER BY employees.name ASC;", cat);

    // Alice (3), Charlie (3), Diana (2)
    ASSERT_EQ(res.result.row_count(), 3u);
    ASSERT_EQ(std::get<std::string>(res.result.rows()[0][0]), "Alice");
    ASSERT_EQ(std::get<int64_t>(res.result.rows()[0][1]), 3);
}

void test_executor_errors() {
    auto emp = create_employees_table();
    snowquery::Catalog cat;
    cat.add_table("employees", &emp);

    // Non-existent column
    ASSERT_THROWS(snowquery::execute_query("SELECT nonexistent FROM employees;", cat), snowquery::SqlError);
    // Non-existent table
    ASSERT_THROWS(snowquery::execute_query("SELECT * FROM bogus_table;", cat), snowquery::SqlError);
}

int main() {
    std::cout << "Running SQL Executor Tests...\n";
    test_simple_select_filter();
    test_where_boolean_logic();
    test_limit();
    test_group_by_aggregates();
    test_hash_join();
    test_executor_errors();

    std::cout << "Results: " << g_tests_passed << "/" << g_tests_run << " passed";
    if (g_tests_failed > 0) {
        std::cout << " (" << g_tests_failed << " FAILED)";
    }
    std::cout << "\n";

    return (g_tests_failed == 0) ? 0 : 1;
}
