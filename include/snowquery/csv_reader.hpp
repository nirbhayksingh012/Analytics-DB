#pragma once
/// @file csv_reader.hpp
/// @brief Reads CSV files into rows of string vectors.
///
/// The CSV reader handles:
///   - Header extraction (first row becomes column names).
///   - Quoted fields (double-quote delimiters, including embedded commas).
///   - Configurable field delimiter (default: comma).
///   - Skipping blank lines.
///   - Reporting parse errors with line numbers.

#include <filesystem>
#include <string>
#include <vector>
#include <stdexcept>

namespace snowquery {

/// Thrown when the CSV reader encounters a problem it cannot recover from.
struct CsvError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// Result of parsing a CSV file.
struct CsvData {
    std::vector<std::string>                headers;  ///< Column names from the first row.
    std::vector<std::vector<std::string>>    rows;     ///< Each inner vector is one data row.
};

/// Configuration knobs for the CSV reader.
struct CsvReaderOptions {
    char delimiter = ',';
    bool has_header = true;   ///< If false, columns are named col_0, col_1, ...
};

/// Parse a CSV file at @p path and return structured data.
/// @throws CsvError on I/O failure or malformed input.
CsvData read_csv(const std::filesystem::path& path,
                 const CsvReaderOptions& opts = {});

/// Parse CSV content from a string (useful for tests).
CsvData read_csv_string(const std::string& content,
                        const CsvReaderOptions& opts = {});

}  // namespace snowquery
