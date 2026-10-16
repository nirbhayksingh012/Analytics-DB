/// @file optimizer.cpp
/// @brief Implementation of query optimizer, planning rules, and EXPLAIN.

#include "snowquery/optimizer.hpp"
#include "snowquery/executor.hpp"

#include <algorithm>
#include <sstream>

namespace snowquery {

// ═══════════════════════════════════════════════════════════════════════
//  HELPER: EXPRESSION TRAVERSAL & COLUMN COLLECTION
// ═══════════════════════════════════════════════════════════════════════

static void collect_columns(const ExprPtr& expr, std::set<std::string>& out_cols) {
    if (!expr) return;
    if (auto* col = dynamic_cast<ColumnRef*>(expr.get())) {
        std::string name = col->column;
        for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        out_cols.insert(name);
        return;
    }
    if (auto* bin = dynamic_cast<BinaryExpr*>(expr.get())) {
        collect_columns(bin->left, out_cols);
        collect_columns(bin->right, out_cols);
        return;
    }
    if (auto* agg = dynamic_cast<AggExpr*>(expr.get())) {
        collect_columns(agg->arg, out_cols);
        return;
    }
}

// ═══════════════════════════════════════════════════════════════════════
//  LOGICAL NODE STRING FORMATTING
// ═══════════════════════════════════════════════════════════════════════

static std::string indent_str(int indent) {
    return std::string(indent * 2, ' ');
}

std::string LogicalScan::to_string(int indent) const {
    std::ostringstream os;
    os << indent_str(indent) << "-> LogicalScan: " << table_name;
    if (!alias.empty() && alias != table_name) os << " AS " << alias;
    os << "\n";

    if (!projected_columns.empty()) {
        os << indent_str(indent + 1) << "* Projected columns: [";
        for (size_t i = 0; i < projected_columns.size(); ++i) {
            if (i > 0) os << ", ";
            os << projected_columns[i];
        }
        os << "] (projection pushdown: APPLIED)\n";
    }

    if (!pushed_predicates.empty()) {
        os << indent_str(indent + 1) << "* Pushed predicates: [";
        for (size_t i = 0; i < pushed_predicates.size(); ++i) {
            if (i > 0) os << " AND ";
            os << expr_to_string(pushed_predicates[i]);
        }
        os << "] (predicate pushdown: APPLIED)\n";
    }

    os << indent_str(indent + 1) << "* Storage data skipping: Min/Max block pruning ENABLED\n";
    return os.str();
}

std::string LogicalFilter::to_string(int indent) const {
    std::ostringstream os;
    os << indent_str(indent) << "-> LogicalFilter: " << expr_to_string(predicate) << "\n";
    if (child) os << child->to_string(indent + 1);
    return os.str();
}

std::string LogicalProject::to_string(int indent) const {
    std::ostringstream os;
    os << indent_str(indent) << "-> LogicalProject: [";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) os << ", ";
        os << expr_to_string(items[i].expr);
        if (!items[i].alias.empty()) os << " AS " << items[i].alias;
    }
    os << "]\n";
    if (child) os << child->to_string(indent + 1);
    return os.str();
}

std::string LogicalAggregate::to_string(int indent) const {
    std::ostringstream os;
    os << indent_str(indent) << "-> LogicalAggregate: ";
    if (!group_by.empty()) {
        os << "GROUP BY [";
        for (size_t i = 0; i < group_by.size(); ++i) {
            if (i > 0) os << ", ";
            os << expr_to_string(group_by[i]);
        }
        os << "] ";
    }
    os << "AGGS [";
    for (size_t i = 0; i < aggregates.size(); ++i) {
        if (i > 0) os << ", ";
        os << expr_to_string(aggregates[i].expr);
        if (!aggregates[i].alias.empty()) os << " AS " << aggregates[i].alias;
    }
    os << "]\n";
    if (child) os << child->to_string(indent + 1);
    return os.str();
}

std::string LogicalSort::to_string(int indent) const {
    std::ostringstream os;
    os << indent_str(indent) << "-> LogicalSort: [";
    for (size_t i = 0; i < order_by.size(); ++i) {
        if (i > 0) os << ", ";
        os << expr_to_string(order_by[i].expr)
           << (order_by[i].dir == SortDir::Desc ? " DESC" : " ASC");
    }
    os << "]\n";
    if (child) os << child->to_string(indent + 1);
    return os.str();
}

std::string LogicalLimit::to_string(int indent) const {
    std::ostringstream os;
    os << indent_str(indent) << "-> LogicalLimit: " << limit_val << "\n";
    if (child) os << child->to_string(indent + 1);
    return os.str();
}

std::string LogicalJoin::to_string(int indent) const {
    std::ostringstream os;
    os << indent_str(indent) << "-> LogicalJoin: " << join_type << " ON "
       << (on_condition ? expr_to_string(on_condition) : "") << "\n";
    if (left) os << left->to_string(indent + 1);
    if (right) os << right->to_string(indent + 1);
    return os.str();
}

// ═══════════════════════════════════════════════════════════════════════
//  PLAN CONSTRUCTION
// ═══════════════════════════════════════════════════════════════════════

LogicalNodePtr QueryOptimizer::build_logical_plan(const SelectStmt& stmt) {
    // Base table scan
    auto scan = std::make_shared<LogicalScan>();
    scan->table_name = stmt.from_table;
    scan->alias = stmt.from_table;

    LogicalNodePtr current = scan;

    // Joins
    for (const auto& join : stmt.joins) {
        auto right_scan = std::make_shared<LogicalScan>();
        right_scan->table_name = join.table_name;
        right_scan->alias = join.alias.empty() ? join.table_name : join.alias;

        auto join_node = std::make_shared<LogicalJoin>();
        join_node->join_type = "INNER";
        join_node->on_condition = join.on_condition;
        join_node->left = current;
        join_node->right = right_scan;
        current = join_node;
    }

    // Filter
    if (stmt.where_clause) {
        auto filter = std::make_shared<LogicalFilter>();
        filter->predicate = stmt.where_clause;
        filter->child = current;
        current = filter;
    }

    // Check for aggregation
    bool has_agg = false;
    for (const auto& item : stmt.select_list) {
        if (dynamic_cast<AggExpr*>(item.expr.get())) {
            has_agg = true;
            break;
        }
    }

    if (has_agg || !stmt.group_by.empty()) {
        auto agg = std::make_shared<LogicalAggregate>();
        agg->group_by = stmt.group_by;
        agg->aggregates = stmt.select_list;
        agg->child = current;
        current = agg;
    } else {
        auto proj = std::make_shared<LogicalProject>();
        proj->items = stmt.select_list;
        proj->child = current;
        current = proj;
    }

    // Sort
    if (!stmt.order_by.empty()) {
        auto sort = std::make_shared<LogicalSort>();
        sort->order_by = stmt.order_by;
        sort->child = current;
        current = sort;
    }

    // Limit
    if (stmt.limit.has_value()) {
        auto limit = std::make_shared<LogicalLimit>();
        limit->limit_val = *stmt.limit;
        limit->child = current;
        current = limit;
    }

    return current;
}

// ═══════════════════════════════════════════════════════════════════════
//  OPTIMIZER RULES
// ═══════════════════════════════════════════════════════════════════════

static void find_scans(const LogicalNodePtr& node, std::vector<std::shared_ptr<LogicalScan>>& scans) {
    if (!node) return;
    if (node->type() == LogicalNodeType::Scan) {
        scans.push_back(std::dynamic_pointer_cast<LogicalScan>(node));
        return;
    }
    if (auto* filt = dynamic_cast<LogicalFilter*>(node.get())) find_scans(filt->child, scans);
    else if (auto* proj = dynamic_cast<LogicalProject*>(node.get())) find_scans(proj->child, scans);
    else if (auto* agg = dynamic_cast<LogicalAggregate*>(node.get())) find_scans(agg->child, scans);
    else if (auto* sort = dynamic_cast<LogicalSort*>(node.get())) find_scans(sort->child, scans);
    else if (auto* lim = dynamic_cast<LogicalLimit*>(node.get())) find_scans(lim->child, scans);
    else if (auto* jn = dynamic_cast<LogicalJoin*>(node.get())) {
        find_scans(jn->left, scans);
        find_scans(jn->right, scans);
    }
}

static void collect_all_referenced_columns(const LogicalNodePtr& node, std::set<std::string>& cols) {
    if (!node) return;
    switch (node->type()) {
        case LogicalNodeType::Filter: {
            auto* f = dynamic_cast<LogicalFilter*>(node.get());
            collect_columns(f->predicate, cols);
            collect_all_referenced_columns(f->child, cols);
            break;
        }
        case LogicalNodeType::Project: {
            auto* p = dynamic_cast<LogicalProject*>(node.get());
            for (const auto& itm : p->items) collect_columns(itm.expr, cols);
            collect_all_referenced_columns(p->child, cols);
            break;
        }
        case LogicalNodeType::Aggregate: {
            auto* a = dynamic_cast<LogicalAggregate*>(node.get());
            for (const auto& gb : a->group_by) collect_columns(gb, cols);
            for (const auto& itm : a->aggregates) collect_columns(itm.expr, cols);
            collect_all_referenced_columns(a->child, cols);
            break;
        }
        case LogicalNodeType::Sort: {
            auto* s = dynamic_cast<LogicalSort*>(node.get());
            for (const auto& ob : s->order_by) collect_columns(ob.expr, cols);
            collect_all_referenced_columns(s->child, cols);
            break;
        }
        case LogicalNodeType::Join: {
            auto* j = dynamic_cast<LogicalJoin*>(node.get());
            collect_columns(j->on_condition, cols);
            collect_all_referenced_columns(j->left, cols);
            collect_all_referenced_columns(j->right, cols);
            break;
        }
        case LogicalNodeType::Limit: {
            auto* l = dynamic_cast<LogicalLimit*>(node.get());
            collect_all_referenced_columns(l->child, cols);
            break;
        }
        case LogicalNodeType::Scan: {
            break;
        }
    }
}

static LogicalNodePtr apply_predicate_pushdown(const LogicalNodePtr& node) {
    if (!node) return nullptr;

    // Recursively optimize children first
    if (auto* filt = dynamic_cast<LogicalFilter*>(node.get())) {
        filt->child = apply_predicate_pushdown(filt->child);
        // If child is a LogicalScan, push predicate directly!
        if (auto scan = std::dynamic_pointer_cast<LogicalScan>(filt->child)) {
            scan->pushed_predicates.push_back(filt->predicate);
            // Redundant filter node is eliminated
            return scan;
        }
    } else if (auto* proj = dynamic_cast<LogicalProject*>(node.get())) {
        proj->child = apply_predicate_pushdown(proj->child);
    } else if (auto* agg = dynamic_cast<LogicalAggregate*>(node.get())) {
        agg->child = apply_predicate_pushdown(agg->child);
    } else if (auto* sort = dynamic_cast<LogicalSort*>(node.get())) {
        sort->child = apply_predicate_pushdown(sort->child);
    } else if (auto* lim = dynamic_cast<LogicalLimit*>(node.get())) {
        lim->child = apply_predicate_pushdown(lim->child);
    } else if (auto* jn = dynamic_cast<LogicalJoin*>(node.get())) {
        jn->left = apply_predicate_pushdown(jn->left);
        jn->right = apply_predicate_pushdown(jn->right);
    }

    return node;
}

LogicalNodePtr QueryOptimizer::optimize(const LogicalNodePtr& root, const Catalog* /*catalog*/) {
    if (!root) return nullptr;

    // Rule 1: Predicate Pushdown
    LogicalNodePtr plan = apply_predicate_pushdown(root);

    // Rule 2: Projection Pushdown / Column Pruning
    std::set<std::string> referenced_columns;
    collect_all_referenced_columns(plan, referenced_columns);

    std::vector<std::shared_ptr<LogicalScan>> scans;
    find_scans(plan, scans);

    for (auto& scan : scans) {
        scan->projected_columns.clear();
        for (const auto& col : referenced_columns) {
            scan->projected_columns.push_back(col);
        }
        std::sort(scan->projected_columns.begin(), scan->projected_columns.end());
    }

    return plan;
}

// ═══════════════════════════════════════════════════════════════════════
//  EXPLAIN OUTPUT
// ═══════════════════════════════════════════════════════════════════════

std::string QueryOptimizer::explain_plan(const LogicalNodePtr& root, const Catalog* catalog) {
    std::ostringstream os;
    os << "=== SnowQuery Optimized Execution Plan ===\n";

    if (catalog) {
        std::vector<std::shared_ptr<LogicalScan>> scans;
        find_scans(root, scans);
        for (const auto& s : scans) {
            auto* tbl = catalog->find_table(s->table_name);
            if (tbl) {
                os << "Estimated relation stats: Table '" << s->table_name << "' has "
                   << tbl->row_count() << " rows, " << tbl->col_count() << " columns.\n";
            }
        }
    }

    os << root->to_string(0);
    os << "===========================================";
    return os.str();
}

}  // namespace snowquery
