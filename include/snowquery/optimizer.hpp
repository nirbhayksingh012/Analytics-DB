#pragma once
/// @file optimizer.hpp
/// @brief Logical and physical query planner, optimizer rules, and EXPLAIN formatter.

#include "sql_parser.hpp"
#include "table.hpp"

#include <memory>
#include <set>
#include <string>
#include <vector>

namespace snowquery {

class Catalog;

enum class LogicalNodeType {
    Scan,
    Filter,
    Project,
    Aggregate,
    Sort,
    Limit,
    Join
};

struct LogicalNode {
    virtual ~LogicalNode() = default;
    virtual LogicalNodeType type() const = 0;
    virtual std::string to_string(int indent = 0) const = 0;
};
using LogicalNodePtr = std::shared_ptr<LogicalNode>;

struct LogicalScan : LogicalNode {
    std::string table_name;
    std::string alias;
    std::vector<std::string> projected_columns;
    std::vector<ExprPtr>     pushed_predicates;

    LogicalNodeType type() const override { return LogicalNodeType::Scan; }
    std::string to_string(int indent = 0) const override;
};

struct LogicalFilter : LogicalNode {
    ExprPtr predicate;
    LogicalNodePtr child;

    LogicalNodeType type() const override { return LogicalNodeType::Filter; }
    std::string to_string(int indent = 0) const override;
};

struct LogicalProject : LogicalNode {
    std::vector<SelectItem> items;
    LogicalNodePtr child;

    LogicalNodeType type() const override { return LogicalNodeType::Project; }
    std::string to_string(int indent = 0) const override;
};

struct LogicalAggregate : LogicalNode {
    std::vector<ExprPtr> group_by;
    std::vector<SelectItem> aggregates;
    LogicalNodePtr child;

    LogicalNodeType type() const override { return LogicalNodeType::Aggregate; }
    std::string to_string(int indent = 0) const override;
};

struct LogicalSort : LogicalNode {
    std::vector<OrderByItem> order_by;
    LogicalNodePtr child;

    LogicalNodeType type() const override { return LogicalNodeType::Sort; }
    std::string to_string(int indent = 0) const override;
};

struct LogicalLimit : LogicalNode {
    int64_t limit_val = 0;
    LogicalNodePtr child;

    LogicalNodeType type() const override { return LogicalNodeType::Limit; }
    std::string to_string(int indent = 0) const override;
};

struct LogicalJoin : LogicalNode {
    std::string join_type = "INNER";
    ExprPtr on_condition;
    LogicalNodePtr left;
    LogicalNodePtr right;

    LogicalNodeType type() const override { return LogicalNodeType::Join; }
    std::string to_string(int indent = 0) const override;
};

/// Query Optimizer: builds logical plans, performs rule-based optimizations, and formats EXPLAIN.
class QueryOptimizer {
public:
    /// Construct the initial logical plan tree from parsed AST.
    static LogicalNodePtr build_logical_plan(const SelectStmt& stmt);

    /// Apply optimization rules:
    /// 1. Predicate pushdown (pushes filters to scan)
    /// 2. Projection pushdown / Column pruning (only scan required columns)
    static LogicalNodePtr optimize(const LogicalNodePtr& root, const Catalog* catalog = nullptr);

    /// Generate detailed human-readable EXPLAIN query plan output.
    static std::string explain_plan(const LogicalNodePtr& root, const Catalog* catalog = nullptr);
};

}  // namespace snowquery
