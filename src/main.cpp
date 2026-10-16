/// @file main.cpp
/// @brief SnowQuery command-line entry point and interactive shell.
///
/// Supports:
///   snowquery --csv <file.csv> [--csv <file2.csv> ...] [--sql "<query>"]
///
/// Features:
///   - Multiple table loading into catalog
///   - Non-interactive SQL query execution via --sql
///   - Interactive shell with multi-line SQL input
///   - Built-in dot-commands: .schema, .tables, .load, .help, .quit

#include "snowquery/csv_reader.hpp"
#include "snowquery/column_store.hpp"
#include "snowquery/executor.hpp"
#include "snowquery/metrics.hpp"
#include "snowquery/sql_parser.hpp"
#include "snowquery/storage.hpp"
#include "snowquery/table.hpp"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

static void print_usage() {
    std::cout <<
        "SnowQuery v0.6.0 — A C++ Analytical Database Engine\n"
        "\n"
        "Usage:\n"
        "  snowquery [--csv <file.csv>] [--storage <file.scol>] [--sql \"<query>\"]\n"
        "  snowquery --help\n"
        "\n"
        "Options:\n"
        "  --csv <path>      Load a CSV table into the catalog.\n"
        "  --storage <path>  Load a persistent columnar file (.scol) into the catalog.\n"
        "  --sql <query>     Execute a SQL statement directly and exit.\n"
        "  --help, -h        Show this help message.\n"
        "\n"
        "Interactive commands:\n"
        "  SELECT ... ;               Execute a SQL query.\n"
        "  EXPLAIN ... ;              Show the optimized query plan.\n"
        "  .tables                    List all loaded tables in the catalog.\n"
        "  .schema [table]            Show schema of loaded table(s).\n"
        "  .load <file.csv|.scol>     Load a CSV or columnar table dynamically.\n"
        "  .save <table_name> <file>  Save table to persistent columnar storage (.scol).\n"
        "  .help                      Show help.\n"
        "  .quit / .exit              Exit the shell.\n"
        "\n";
}

int main(int argc, char* argv[]) {
    std::vector<std::string> csv_paths;
    std::string sql_query;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            print_usage();
            return 0;
        }
        if (std::strcmp(argv[i], "--csv") == 0 && i + 1 < argc) {
            csv_paths.push_back(argv[++i]);
        } else if (std::strcmp(argv[i], "--storage") == 0 && i + 1 < argc) {
            csv_paths.push_back(argv[++i]);
        } else if (std::strcmp(argv[i], "--sql") == 0 && i + 1 < argc) {
            sql_query = argv[++i];
        }
    }

    // If no tables loaded initially, start empty shell


    // Catalog holding all in-memory tables.
    snowquery::Catalog catalog;
    std::map<std::string, std::unique_ptr<snowquery::Table>> table_storage;

    auto load_table = [&](const std::string& path) -> bool {
        snowquery::QueryMetrics load_metrics;
        load_metrics.query_text = "LOAD " + path;
        snowquery::ScopedTimer timer(load_metrics.total_time);

        try {
            std::filesystem::path fpath(path);
            std::string ext = fpath.extension().string();
            std::string name = fpath.stem().string();
            std::string lower_name = name;
            for (char& c : lower_name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

            std::unique_ptr<snowquery::Table> tbl;
            if (ext == ".scol" || ext == ".snow") {
                tbl = std::make_unique<snowquery::Table>(snowquery::ColumnarStorage::read_file(path));
            } else {
                auto csv = snowquery::read_csv(path);
                tbl = std::make_unique<snowquery::Table>(
                    snowquery::Table::from_csv(name, csv.headers, csv.rows));
            }

            load_metrics.rows_examined = tbl->row_count();
            load_metrics.rows_returned = tbl->row_count();

            catalog.add_table(lower_name, tbl.get());
            table_storage[lower_name] = std::move(tbl);

            std::cout << "Loaded table '" << name << "' ("
                      << table_storage[lower_name]->row_count() << " rows, "
                      << table_storage[lower_name]->col_count() << " columns) in "
                      << load_metrics.total_time_ms() << " ms.\n";
            return true;
        } catch (const std::exception& e) {
            std::cerr << "Error loading table '" << path << "': " << e.what() << "\n";
            return false;
        }
    };


    for (const auto& path : csv_paths) {
        if (!load_table(path)) {
            return 1;
        }
    }

    // Non-interactive mode via --sql
    if (!sql_query.empty()) {
        try {
            auto res = snowquery::execute_query(sql_query, catalog);
            if (!res.explain_text.empty()) {
                std::cout << res.explain_text << "\n";
            } else {
                res.result.print(std::cout);
                std::cout << "\n";
                res.metrics.print(std::cout);
            }
            return 0;
        } catch (const std::exception& e) {
            std::cerr << "Query error: " << e.what() << "\n";
            return 1;
        }
    }

    // ── Interactive loop ──────────────────────────────────────────
    std::cout << "\nSnowQuery Interactive Shell. Type SQL statements ending with ';' or '.help'.\n";
    std::cout << "snowquery> ";

    std::string sql_buffer;
    std::string line;

    while (std::getline(std::cin, line)) {
        auto start = line.find_first_not_of(" \t");
        if (start == std::string::npos && sql_buffer.empty()) {
            std::cout << "snowquery> ";
            continue;
        }
        std::string trimmed = (start == std::string::npos) ? "" : line.substr(start);

        // Dot commands (only when not buffering SQL)
        if (sql_buffer.empty() && !trimmed.empty() && trimmed[0] == '.') {
            if (trimmed == ".quit" || trimmed == ".exit") {
                break;
            } else if (trimmed == ".help") {
                print_usage();
            } else if (trimmed == ".tables") {
                std::cout << "Catalog tables:\n";
                for (const auto& name : catalog.table_names()) {
                    auto* tbl = catalog.find_table(name);
                    std::cout << "  " << name << " (" << (tbl ? tbl->row_count() : 0) << " rows)\n";
                }
            } else if (trimmed.rfind(".schema", 0) == 0) {
                std::string target;
                if (trimmed.size() > 7) {
                    auto p = trimmed.find_first_not_of(" \t", 7);
                    if (p != std::string::npos) target = trimmed.substr(p);
                }
                for (char& c : target) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

                if (!target.empty()) {
                    auto* tbl = catalog.find_table(target);
                    if (!tbl) {
                        std::cerr << "Table not found: " << target << "\n";
                    } else {
                        std::cout << "Table: " << tbl->name() << "\n";
                        for (const auto& col : tbl->schema()) {
                            std::cout << "  " << col.name << " "
                                      << snowquery::data_type_name(col.type) << "\n";
                        }
                    }
                } else {
                    for (const auto& name : catalog.table_names()) {
                        auto* tbl = catalog.find_table(name);
                        std::cout << "Table: " << (tbl ? tbl->name() : name) << "\n";
                        if (tbl) {
                            for (const auto& col : tbl->schema()) {
                                std::cout << "  " << col.name << " "
                                          << snowquery::data_type_name(col.type) << "\n";
                            }
                        }
                        std::cout << "\n";
                    }
                }
            } else if (trimmed.rfind(".load", 0) == 0) {
                auto p = trimmed.find_first_not_of(" \t", 5);
                if (p == std::string::npos) {
                    std::cerr << "Usage: .load <file.csv|.scol>\n";
                } else {
                    std::string path = trimmed.substr(p);
                    load_table(path);
                }
            } else if (trimmed.rfind(".save", 0) == 0) {
                std::istringstream iss(trimmed.substr(5));
                std::string tname, spath;
                if (!(iss >> tname >> spath)) {
                    std::cerr << "Usage: .save <table_name> <file.scol>\n";
                } else {
                    auto* tbl = catalog.find_table(tname);
                    if (!tbl) {
                        std::cerr << "Table not found: " << tname << "\n";
                    } else {
                        try {
                            snowquery::ColumnarStorage::write_file(spath, *tbl);
                            std::cout << "Saved table '" << tname << "' (" << tbl->row_count()
                                      << " rows) to '" << spath << "'.\n";
                        } catch (const std::exception& e) {
                            std::cerr << "Error saving table: " << e.what() << "\n";
                        }
                    }
                }
            } else {
                std::cerr << "Unknown command: " << trimmed << ". Type .help for available commands.\n";
            }

            std::cout << "snowquery> ";
            continue;
        }

        // Accumulate SQL query
        if (!sql_buffer.empty()) sql_buffer += " ";
        sql_buffer += trimmed;

        // Check if query is complete (ends with ';')
        if (!sql_buffer.empty() && sql_buffer.back() == ';') {
            try {
                auto res = snowquery::execute_query(sql_buffer, catalog);
                if (!res.explain_text.empty()) {
                    std::cout << res.explain_text << "\n";
                } else {
                    res.result.print(std::cout);
                    std::cout << "\n";
                    res.metrics.print(std::cout);
                }
            } catch (const std::exception& e) {
                std::cerr << "Error: " << e.what() << "\n";
            }
            sql_buffer.clear();
            std::cout << "snowquery> ";
        } else {
            std::cout << "   ...> ";
        }
    }

    std::cout << "Goodbye.\n";
    return 0;
}
