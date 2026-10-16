#pragma once
/// @file executor.hpp
/// @brief Query executor — evaluates parsed SQL against loaded tables.
///
/// The executor takes a SelectStmt AST and a "catalog" of loaded tables,
/// and produces a result Table.

#include "sql_parser.hpp"
#include "table.hpp"
#include "metrics.hpp"
#include <string>
#include <unordered_map>
#include <functional>

namespace snowquery {

/// A simple catalog mapping table names (lowercase) to Table pointers.
class Catalog {
public:
    void add_table(const std::string& name, const Table* table);
    const Table* find_table(const std::string& name) const;
    std::vector<std::string> table_names() const;
private:
    std::unordered_map<std::string, const Table*> tables_;
};

/// Execute result.
struct ExecResult {
    Table         result;
    QueryMetrics  metrics;
    std::string   explain_text;  // non-empty if EXPLAIN was requested
};

/// Execute a SQL query against the catalog.
ExecResult execute_query(const std::string& sql, const Catalog& catalog);

/// Execute a pre-parsed statement against the catalog.
ExecResult execute_stmt(const SelectStmt& stmt, const Catalog& catalog,
                        const std::string& sql_text = "");

// ── Shared evaluation helpers ─────────────────────────────────────────
int resolve_column(const ColumnRef& ref, const std::vector<ColumnDef>& schema,
                   const std::string& table_alias = "");
Value eval_expr(const ExprPtr& expr, const std::vector<ColumnDef>& schema,
                const Row& row);
bool eval_predicate(const ExprPtr& expr, const std::vector<ColumnDef>& schema,
                    const Row& row);
int compare_values(const Value& a, const Value& b);
std::string value_to_sort_string(const Value& v);
double value_to_double(const Value& v);

}  // namespace snowquery

