/// @file test_table.cpp
/// @brief Automated tests for Table, type inference, and ColumnStore.

#include "snowquery/column_store.hpp"
#include "snowquery/table.hpp"

#include <iostream>
#include <sstream>
#include <cstdlib>

// ── Minimal test harness ──────────────────────────────────────────────

static int g_tests_run    = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

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

// ── Tests ─────────────────────────────────────────────────────────────

void test_type_inference() {
    ASSERT_EQ(snowquery::infer_type({"1", "2", "3"}),
              snowquery::DataType::Integer);
    ASSERT_EQ(snowquery::infer_type({"1.5", "2.0", "3.14"}),
              snowquery::DataType::Float);
    ASSERT_EQ(snowquery::infer_type({"hello", "world"}),
              snowquery::DataType::Text);
    ASSERT_EQ(snowquery::infer_type({"", "", ""}),
              snowquery::DataType::Null);
    // Mix of int and float → Float.
    ASSERT_EQ(snowquery::infer_type({"1", "2.5"}),
              snowquery::DataType::Float);
}

void test_parse_value() {
    auto v1 = snowquery::parse_value("42", snowquery::DataType::Integer);
    ASSERT_TRUE(std::holds_alternative<int64_t>(v1));
    ASSERT_EQ(std::get<int64_t>(v1), int64_t{42});

    auto v2 = snowquery::parse_value("3.14", snowquery::DataType::Float);
    ASSERT_TRUE(std::holds_alternative<double>(v2));

    auto v3 = snowquery::parse_value("hello", snowquery::DataType::Text);
    ASSERT_TRUE(std::holds_alternative<std::string>(v3));
    ASSERT_EQ(std::get<std::string>(v3), std::string("hello"));

    auto v4 = snowquery::parse_value("", snowquery::DataType::Integer);
    ASSERT_TRUE(std::holds_alternative<std::monostate>(v4));
}

void test_table_from_csv() {
    std::vector<std::string> headers = {"id", "name", "score"};
    std::vector<std::vector<std::string>> rows = {
        {"1", "Alice", "95.5"},
        {"2", "Bob",   "88.0"},
        {"3", "Charlie", "92.3"},
    };

    auto table = snowquery::Table::from_csv("test", headers, rows);

    ASSERT_EQ(table.name(), std::string("test"));
    ASSERT_EQ(table.row_count(), size_t{3});
    ASSERT_EQ(table.col_count(), size_t{3});

    // Check inferred types.
    ASSERT_EQ(table.schema()[0].type, snowquery::DataType::Integer);
    ASSERT_EQ(table.schema()[1].type, snowquery::DataType::Text);
    ASSERT_EQ(table.schema()[2].type, snowquery::DataType::Float);

    // Check values.
    ASSERT_EQ(std::get<int64_t>(table.rows()[0][0]), int64_t{1});
    ASSERT_EQ(std::get<std::string>(table.rows()[1][1]), std::string("Bob"));
}

void test_column_index() {
    std::vector<std::string> headers = {"ID", "Name"};
    std::vector<std::vector<std::string>> rows = {{"1", "A"}};
    auto table = snowquery::Table::from_csv("t", headers, rows);

    // Case-insensitive lookup.
    ASSERT_EQ(table.column_index("id"), 0);
    ASSERT_EQ(table.column_index("NAME"), 1);
    ASSERT_EQ(table.column_index("missing"), -1);
}

void test_column_store() {
    std::vector<std::string> headers = {"x", "y"};
    std::vector<std::vector<std::string>> rows = {
        {"1", "a"},
        {"2", "b"},
        {"3", "c"},
    };
    auto table = snowquery::Table::from_csv("t", headers, rows);
    auto cs = snowquery::ColumnStore::from_table(table);

    ASSERT_EQ(cs.row_count(), size_t{3});
    ASSERT_EQ(cs.col_count(), size_t{2});
    ASSERT_EQ(cs.column_index("x"), 0);
    ASSERT_EQ(cs.column_index("Y"), 1);

    // Verify first column values.
    ASSERT_EQ(std::get<int64_t>(cs.columns()[0].data[0]), int64_t{1});
    ASSERT_EQ(std::get<int64_t>(cs.columns()[0].data[2]), int64_t{3});
}

void test_table_print() {
    std::vector<std::string> headers = {"id", "val"};
    std::vector<std::vector<std::string>> rows = {{"1", "hi"}};
    auto table = snowquery::Table::from_csv("t", headers, rows);

    std::ostringstream out;
    table.print(out);
    std::string output = out.str();

    // Should contain the header and the value.
    ASSERT_TRUE(output.find("id") != std::string::npos);
    ASSERT_TRUE(output.find("hi") != std::string::npos);
    ASSERT_TRUE(output.find("1 of 1") != std::string::npos);
}

// ── Entry point ───────────────────────────────────────────────────────

int main() {
    test_type_inference();
    test_parse_value();
    test_table_from_csv();
    test_column_index();
    test_column_store();
    test_table_print();

    std::cout << "\n=== Table & ColumnStore Tests ===\n"
              << "  Passed: " << g_tests_passed << "\n"
              << "  Failed: " << g_tests_failed << "\n"
              << "  Total:  " << g_tests_run    << "\n\n";

    return g_tests_failed > 0 ? 1 : 0;
}
