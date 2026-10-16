/// @file csv_reader.cpp
/// @brief Implementation of the CSV reader.

#include "snowquery/csv_reader.hpp"

#include <fstream>
#include <sstream>

namespace snowquery {

// ── Internal helpers ──────────────────────────────────────────────────

namespace {

/// Parse a single line (or continuation of a quoted field spanning
/// multiple lines) into a vector of field strings.
///
/// The parser is a small state machine:
///   - Outside quotes: commas separate fields; a double-quote opens a
///     quoted field.
///   - Inside quotes: commas are literal; a double-quote either ends the
///     field or, if followed by another double-quote, represents a
///     literal '"' character.
///
/// @param lines     all lines of the CSV content
/// @param line_idx  in/out – current line index (advanced past any
///                  continuation lines consumed by multi-line quoted fields)
/// @param delim     the field delimiter character
/// @returns         a vector of field strings for one logical row
std::vector<std::string> parse_row(const std::vector<std::string>& lines,
                                   size_t& line_idx, char delim)
{
    std::vector<std::string> fields;
    std::string field;
    bool in_quotes = false;

    for (; line_idx < lines.size(); ++line_idx) {
        const std::string& line = lines[line_idx];

        for (size_t i = 0; i < line.size(); ++i) {
            char ch = line[i];

            if (in_quotes) {
                if (ch == '"') {
                    // Peek ahead: doubled quote → literal '"'
                    if (i + 1 < line.size() && line[i + 1] == '"') {
                        field += '"';
                        ++i;  // skip the second quote
                    } else {
                        in_quotes = false;  // closing quote
                    }
                } else {
                    field += ch;
                }
            } else {
                if (ch == '"') {
                    in_quotes = true;
                } else if (ch == delim) {
                    fields.push_back(std::move(field));
                    field.clear();
                } else {
                    field += ch;
                }
            }
        }

        if (!in_quotes) {
            // Row ends here.
            fields.push_back(std::move(field));
            return fields;
        }
        // Still inside quotes → the field spans another line.
        field += '\n';
    }

    // If we reach the end of input while still inside quotes, the file
    // is malformed.
    if (in_quotes) {
        throw CsvError("unterminated quoted field at end of input");
    }
    fields.push_back(std::move(field));
    return fields;
}

/// Trim leading and trailing whitespace from a string.
std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

}  // anonymous namespace

// ── Public API ────────────────────────────────────────────────────────

CsvData read_csv_string(const std::string& content,
                        const CsvReaderOptions& opts)
{
    // Split content into lines.
    std::vector<std::string> lines;
    {
        std::istringstream ss(content);
        std::string line;
        while (std::getline(ss, line)) {
            // Remove trailing \r (Windows line endings).
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            lines.push_back(std::move(line));
        }
    }

    CsvData result;
    size_t idx = 0;

    // Skip leading blank lines.
    while (idx < lines.size() && trim(lines[idx]).empty()) ++idx;

    if (idx >= lines.size()) {
        throw CsvError("CSV input is empty");
    }

    // Parse header row.
    if (opts.has_header) {
        result.headers = parse_row(lines, idx, opts.delimiter);
        for (auto& h : result.headers) h = trim(h);
        ++idx;  // advance past header row
    }

    // Parse data rows.
    while (idx < lines.size()) {
        if (trim(lines[idx]).empty()) {
            ++idx;
            continue;  // skip blank lines
        }

        auto row = parse_row(lines, idx, opts.delimiter);
        ++idx;

        // If header was present, ensure field count matches.
        if (opts.has_header && row.size() != result.headers.size()) {
            throw CsvError(
                "row has " + std::to_string(row.size()) +
                " fields but header has " +
                std::to_string(result.headers.size()) +
                " columns (near line " + std::to_string(idx) + ")");
        }

        // Generate synthetic headers on the first data row if needed.
        if (!opts.has_header && result.headers.empty()) {
            for (size_t c = 0; c < row.size(); ++c) {
                result.headers.push_back("col_" + std::to_string(c));
            }
        }

        // Trim each field value.
        for (auto& f : row) f = trim(f);
        result.rows.push_back(std::move(row));
    }

    return result;
}

CsvData read_csv(const std::filesystem::path& path,
                 const CsvReaderOptions& opts)
{
    std::ifstream file(path);
    if (!file.is_open()) {
        throw CsvError("cannot open file: " + path.string());
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    return read_csv_string(ss.str(), opts);
}

}  // namespace snowquery
