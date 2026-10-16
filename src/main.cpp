/// @file main.cpp
/// @brief SnowQuery command-line entry point (Phase 1).
///
/// Usage:
///   snowquery --csv data/orders.csv
///
/// Loads a CSV file, infers types, and prints the table.

#include "snowquery/csv_reader.hpp"
#include "snowquery/column_store.hpp"
#include "snowquery/metrics.hpp"
#include "snowquery/table.hpp"

#include <chrono>
#include <cstring>
#include <iostream>
#include <string>

static void print_usage() {
    std::cout <<
        "SnowQuery v0.1.0 — A C++ Analytical Database Engine\n"
        "\n"
        "Usage:\n"
        "  snowquery --csv <file.csv>   Load a CSV file and start interactive mode.\n"
        "  snowquery --help             Show this help message.\n"
        "\n"
        "Interactive commands (available after loading a table):\n"
        "  .schema                      Show table schema.\n"
        "  .quit / .exit                Exit the shell.\n"
        "\n";
}

int main(int argc, char* argv[]) {
    // ── Parse command-line arguments ─────────────────────────────
    std::string csv_path;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            print_usage();
            return 0;
        }
        if (std::strcmp(argv[i], "--csv") == 0 && i + 1 < argc) {
            csv_path = argv[++i];
        }
    }

    if (csv_path.empty()) {
        print_usage();
        return 1;
    }

    // ── Load CSV ─────────────────────────────────────────────────
    snowquery::QueryMetrics load_metrics;
    load_metrics.query_text = "LOAD CSV " + csv_path;

    snowquery::Table table("", {});
    {
        snowquery::ScopedTimer timer(load_metrics.total_time);
        try {
            auto csv = snowquery::read_csv(csv_path);
            // Derive a table name from the filename (e.g. "orders" from "orders.csv").
            std::string name = std::filesystem::path(csv_path).stem().string();
            table = snowquery::Table::from_csv(name, csv.headers, csv.rows);
            load_metrics.rows_examined = table.row_count();
            load_metrics.rows_returned = table.row_count();
        } catch (const snowquery::CsvError& e) {
            std::cerr << "Error loading CSV: " << e.what() << "\n";
            return 1;
        }
    }

    std::cout << "\nLoaded table '" << table.name() << "' ("
              << table.row_count() << " rows, "
              << table.col_count() << " columns).\n\n";

    table.print(std::cout);
    std::cout << "\n";
    load_metrics.print(std::cout);

    // ── Build columnar representation ────────────────────────────
    auto col_store = snowquery::ColumnStore::from_table(table);
    std::cout << "\nColumnar store:\n";
    col_store.print_info(std::cout);

    // ── Interactive loop (Phase 1: dot-commands only) ────────────
    std::cout << "\nsnowquery> ";
    std::string line;
    while (std::getline(std::cin, line)) {
        // Trim.
        auto start = line.find_first_not_of(" \t");
        if (start == std::string::npos) { std::cout << "snowquery> "; continue; }
        line = line.substr(start);

        if (line == ".quit" || line == ".exit") break;

        if (line == ".schema") {
            std::cout << "Table: " << table.name() << "\n";
            for (const auto& col : table.schema()) {
                std::cout << "  " << col.name << " "
                          << snowquery::data_type_name(col.type) << "\n";
            }
        } else {
            std::cout << "Unknown command or SQL not yet supported: " << line << "\n"
                      << "Use .schema, .quit, or wait for Phase 2 SQL support.\n";
        }
        std::cout << "snowquery> ";
    }

    std::cout << "Goodbye.\n";
    return 0;
}
