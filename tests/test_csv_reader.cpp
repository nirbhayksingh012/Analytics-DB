/// @file test_csv_reader.cpp
/// @brief Automated tests for the CSV reader.
///
/// Uses a minimal assertion macro — no external test framework needed.

#include "snowquery/csv_reader.hpp"

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

#define ASSERT_THROW(expr, exc_type)                                          \
    do {                                                                        \
        ++g_tests_run;                                                         \
        bool caught = false;                                                   \
        try { expr; } catch (const exc_type&) { caught = true; }               \
        if (caught) {                                                          \
            ++g_tests_passed;                                                  \
        } else {                                                               \
            ++g_tests_failed;                                                  \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__                \
                      << "  expected " #exc_type " from " #expr << "\n";       \
        }                                                                      \
    } while (false)

// ── Tests ─────────────────────────────────────────────────────────────

void test_basic_csv() {
    std::string csv =
        "name,age,score\n"
        "Alice,30,95.5\n"
        "Bob,25,88.0\n";

    auto data = snowquery::read_csv_string(csv);

    ASSERT_EQ(data.headers.size(), size_t{3});
    ASSERT_EQ(data.headers[0], "name");
    ASSERT_EQ(data.headers[1], "age");
    ASSERT_EQ(data.headers[2], "score");

    ASSERT_EQ(data.rows.size(), size_t{2});
    ASSERT_EQ(data.rows[0][0], "Alice");
    ASSERT_EQ(data.rows[0][1], "30");
    ASSERT_EQ(data.rows[1][0], "Bob");
}

void test_quoted_fields() {
    // CSV content with a field containing a comma and a field with escaped quotes.
    // In CSV, a doubled quote ("") inside a quoted field represents a literal ".
    std::string csv =
        R"(name,address
Alice,"123 Main St, Apt 4"
Bob,"456 Oak ""Ave"""
)";

    auto data = snowquery::read_csv_string(csv);

    ASSERT_EQ(data.rows.size(), size_t{2});
    ASSERT_EQ(data.rows[0][1], "123 Main St, Apt 4");
    ASSERT_EQ(data.rows[1][1], std::string("456 Oak \"Ave\""));
}

void test_blank_lines_skipped() {
    std::string csv =
        "x,y\n"
        "\n"
        "1,2\n"
        "\n"
        "3,4\n";

    auto data = snowquery::read_csv_string(csv);
    ASSERT_EQ(data.rows.size(), size_t{2});
    ASSERT_EQ(data.rows[0][0], "1");
    ASSERT_EQ(data.rows[1][0], "3");
}

void test_no_header() {
    std::string csv = "a,b,c\n1,2,3\n";

    snowquery::CsvReaderOptions opts;
    opts.has_header = false;

    auto data = snowquery::read_csv_string(csv, opts);

    ASSERT_EQ(data.headers.size(), size_t{3});
    ASSERT_EQ(data.headers[0], "col_0");
    ASSERT_EQ(data.rows.size(), size_t{2});
}

void test_custom_delimiter() {
    std::string csv = "a|b|c\n1|2|3\n";

    snowquery::CsvReaderOptions opts;
    opts.delimiter = '|';

    auto data = snowquery::read_csv_string(csv, opts);
    ASSERT_EQ(data.headers[1], "b");
    ASSERT_EQ(data.rows[0][2], "3");
}

void test_empty_input_throws() {
    ASSERT_THROW(
        snowquery::read_csv_string(""),
        snowquery::CsvError
    );
}

void test_field_count_mismatch_throws() {
    std::string csv =
        "a,b\n"
        "1,2,3\n";

    ASSERT_THROW(
        snowquery::read_csv_string(csv),
        snowquery::CsvError
    );
}

void test_windows_line_endings() {
    std::string csv = "x,y\r\n1,2\r\n3,4\r\n";

    auto data = snowquery::read_csv_string(csv);
    ASSERT_EQ(data.rows.size(), size_t{2});
    ASSERT_EQ(data.rows[0][0], "1");
}

void test_whitespace_trimming() {
    std::string csv =
        " name , value \n"
        " Alice , 42 \n";

    auto data = snowquery::read_csv_string(csv);
    ASSERT_EQ(data.headers[0], "name");
    ASSERT_EQ(data.headers[1], "value");
    ASSERT_EQ(data.rows[0][0], "Alice");
    ASSERT_EQ(data.rows[0][1], "42");
}

// ── Entry point ───────────────────────────────────────────────────────

int main() {
    test_basic_csv();
    test_quoted_fields();
    test_blank_lines_skipped();
    test_no_header();
    test_custom_delimiter();
    test_empty_input_throws();
    test_field_count_mismatch_throws();
    test_windows_line_endings();
    test_whitespace_trimming();

    std::cout << "\n=== CSV Reader Tests ===\n"
              << "  Passed: " << g_tests_passed << "\n"
              << "  Failed: " << g_tests_failed << "\n"
              << "  Total:  " << g_tests_run    << "\n\n";

    return g_tests_failed > 0 ? 1 : 0;
}
