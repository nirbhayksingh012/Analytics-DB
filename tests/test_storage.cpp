/// @file test_storage.cpp
/// @brief Automated tests for Persistent Columnar Storage (.scol), block pruning, and file validation.

#include "snowquery/storage.hpp"
#include "snowquery/table.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
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

const std::string TEST_DIR = "test_data_tmp";

static void setup_test_dir() {
    std::filesystem::create_directories(TEST_DIR);
}

static void cleanup_test_dir() {
    std::error_code ec;
    std::filesystem::remove_all(TEST_DIR, ec);
}

void test_storage_roundtrip() {
    std::vector<snowquery::ColumnDef> schema = {
        {"id", snowquery::DataType::Integer},
        {"name", snowquery::DataType::Text},
        {"score", snowquery::DataType::Float}
    };

    std::vector<snowquery::Row> rows = {
        {int64_t{1}, std::string("Alice"), 95.5},
        {int64_t{2}, std::string("Bob"),   88.0},
        {int64_t{3}, std::monostate{},     72.5},  // null text
        {int64_t{4}, std::string("Diana"), std::monostate{}} // null float
    };

    snowquery::Table original("students", schema, rows);
    std::string path = TEST_DIR + "/students.scol";

    snowquery::ColumnarStorage::write_file(path, original, 2); // 2 rows per block -> 2 blocks

    snowquery::Table loaded = snowquery::ColumnarStorage::read_file(path);

    ASSERT_EQ(loaded.name(), "students");
    ASSERT_EQ(loaded.row_count(), 4u);
    ASSERT_EQ(loaded.col_count(), 3u);

    // Verify row 0
    ASSERT_EQ(std::get<int64_t>(loaded.rows()[0][0]), 1);
    ASSERT_EQ(std::get<std::string>(loaded.rows()[0][1]), "Alice");
    ASSERT_EQ(std::get<double>(loaded.rows()[0][2]), 95.5);

    // Verify nulls
    ASSERT_TRUE(std::holds_alternative<std::monostate>(loaded.rows()[2][1]));
    ASSERT_TRUE(std::holds_alternative<std::monostate>(loaded.rows()[3][2]));
}

void test_corrupted_and_truncated_files() {
    std::string bad_magic_path = TEST_DIR + "/bad_magic.scol";
    {
        std::ofstream os(bad_magic_path, std::ios::binary);
        uint32_t fake_magic = 0x12345678;
        os.write(reinterpret_cast<char*>(&fake_magic), sizeof(fake_magic));
    }
    ASSERT_THROWS(snowquery::ColumnarStorage::read_file(bad_magic_path), std::runtime_error);

    std::string truncated_path = TEST_DIR + "/truncated.scol";
    {
        std::ofstream os(truncated_path, std::ios::binary);
        uint32_t magic = snowquery::ColumnarStorage::MAGIC;
        os.write(reinterpret_cast<char*>(&magic), sizeof(magic));
        // file ends abruptly without version or headers
    }
    ASSERT_THROWS(snowquery::ColumnarStorage::read_file(truncated_path), std::runtime_error);
}

void test_block_pruning() {
    std::vector<snowquery::ColumnDef> schema = {
        {"val", snowquery::DataType::Integer},
        {"label", snowquery::DataType::Text}
    };

    // 1000 rows, block size 100 -> 10 blocks:
    // Block 0: 0..99
    // Block 1: 100..199
    // ...
    // Block 9: 900..999
    std::vector<snowquery::Row> rows;
    for (int64_t i = 0; i < 1000; ++i) {
        rows.push_back({i, "Item_" + std::to_string(i)});
    }

    snowquery::Table table("numbers", schema, rows);
    std::string path = TEST_DIR + "/numbers.scol";
    snowquery::ColumnarStorage::write_file(path, table, 100);

    // Query 1: val >= 850 (matches blocks 8 and 9, skips blocks 0..7)
    auto pred1 = std::make_shared<snowquery::BinaryExpr>();
    pred1->op = snowquery::BinaryExpr::Ge;
    auto col_ref = std::make_shared<snowquery::ColumnRef>();
    col_ref->column = "val";
    auto lit = std::make_shared<snowquery::IntLit>();
    lit->value = 850;
    pred1->left = col_ref;
    pred1->right = lit;

    snowquery::QueryMetrics metrics;
    snowquery::Table filtered = snowquery::ColumnarStorage::read_file_filtered(
        path, {"val"}, pred1, &metrics);

    ASSERT_EQ(metrics.blocks_skipped, 8u);
    ASSERT_EQ(metrics.blocks_examined, 2u);
    ASSERT_EQ(filtered.row_count(), 200u); // read 2 blocks = 200 rows into memory

    // Query 2: val < 50 (matches only block 0, skips blocks 1..9)
    auto pred2 = std::make_shared<snowquery::BinaryExpr>();
    pred2->op = snowquery::BinaryExpr::Lt;
    pred2->left = col_ref;
    auto lit2 = std::make_shared<snowquery::IntLit>();
    lit2->value = 50;
    pred2->right = lit2;

    snowquery::QueryMetrics metrics2;
    snowquery::Table filtered2 = snowquery::ColumnarStorage::read_file_filtered(
        path, {"val"}, pred2, &metrics2);

    ASSERT_EQ(metrics2.blocks_skipped, 9u);
    ASSERT_EQ(metrics2.blocks_examined, 1u);
    ASSERT_EQ(filtered2.row_count(), 100u);
}

int main() {
    std::cout << "Running Columnar Storage Tests...\n";
    setup_test_dir();

    test_storage_roundtrip();
    test_corrupted_and_truncated_files();
    test_block_pruning();

    cleanup_test_dir();

    std::cout << "Results: " << g_tests_passed << "/" << g_tests_run << " passed";
    if (g_tests_failed > 0) {
        std::cout << " (" << g_tests_failed << " FAILED)";
    }
    std::cout << "\n";

    return (g_tests_failed == 0) ? 0 : 1;
}
