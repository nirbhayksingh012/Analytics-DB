#pragma once
/// @file sql_parser.hpp
/// @brief SQL tokenizer, parser, and AST for SnowQuery's supported SQL subset.
///
/// Supported SQL:
///   SELECT [columns | * | aggregates] FROM table
///   [WHERE condition]
///   [GROUP BY columns]
///   [ORDER BY columns [ASC|DESC]]
///   [LIMIT n]
///
/// Aggregates: COUNT(*), COUNT(col), SUM(col), MIN(col), MAX(col)
/// Operators: =, !=, <>, <, >, <=, >=
/// Logical: AND, OR with correct precedence; parentheses
/// Literals: integers, floats, single-quoted strings

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace snowquery {

// ═══════════════════════════════════════════════════════════════════════
//  TOKENIZER
// ═══════════════════════════════════════════════════════════════════════

enum class TokenType {
    // Keywords
    Select, From, Where, And, Or, Not,
    Order, By, Asc, Desc,
    Limit, Group,
    Count, Sum, Min, Max,
    As, On, Join, Inner, Left, Right, Cross,
    Explain,

    // Identifiers & literals
    Identifier,    // column or table name
    IntLiteral,    // e.g. 42
    FloatLiteral,  // e.g. 3.14
    StringLiteral, // e.g. 'hello'

    // Operators
    Star,      // *
    Comma,     // ,
    Dot,       // .
    LParen,    // (
    RParen,    // )
    Semicolon, // ;
    Eq,        // =
    Neq,       // != or <>
    Lt,        // <
    Gt,        // >
    Le,        // <=
    Ge,        // >=

    Eof
};

struct Token {
    TokenType   type;
    std::string text;
    size_t      pos = 0;  // character offset in the input string
};

/// Thrown on parse or tokenizer error.
struct SqlError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// Tokenize a SQL string into a vector of tokens.
std::vector<Token> tokenize(const std::string& sql);

// ═══════════════════════════════════════════════════════════════════════
//  AST NODES
// ═══════════════════════════════════════════════════════════════════════

/// Supported aggregate functions.
enum class AggFunc { Count, Sum, Min, Max };
std::string agg_func_name(AggFunc f);

/// Forward-declare expression types for shared_ptr usage.
struct Expr;
using ExprPtr = std::shared_ptr<Expr>;

/// An expression node in the AST.
struct Expr {
    virtual ~Expr() = default;
};

/// Column reference: optionally qualified (table.column).
struct ColumnRef : Expr {
    std::string table;   // empty if unqualified
    std::string column;
};

/// Integer literal.
struct IntLit : Expr {
    int64_t value;
};

/// Float literal.
struct FloatLit : Expr {
    double value;
};

/// String literal.
struct StringLit : Expr {
    std::string value;
};

/// Star expression (*) — used in SELECT * and COUNT(*).
struct StarExpr : Expr {};

/// Aggregate function call: COUNT(*), SUM(col), etc.
struct AggExpr : Expr {
    AggFunc   func;
    ExprPtr   arg;       // StarExpr for COUNT(*), ColumnRef for others
};

/// Binary comparison: col op literal, or expr op expr.
struct BinaryExpr : Expr {
    enum Op { Eq, Neq, Lt, Gt, Le, Ge, And, Or };
    Op       op;
    ExprPtr  left;
    ExprPtr  right;
};

/// A single item in the SELECT list.
struct SelectItem {
    ExprPtr     expr;
    std::string alias;   // optional AS alias
};

/// Sort direction.
enum class SortDir { Asc, Desc };

/// A single ORDER BY key.
struct OrderByItem {
    ExprPtr  expr;
    SortDir  dir = SortDir::Asc;
};

/// Join specification.
struct JoinClause {
    std::string table_name;
    std::string alias;
    ExprPtr     on_condition;
};

/// The complete parsed SELECT statement.
struct SelectStmt {
    bool                       is_explain = false;
    std::vector<SelectItem>    select_list;
    std::string                from_table;
    std::string                from_alias;
    std::vector<JoinClause>    joins;
    ExprPtr                    where_clause;
    std::vector<ExprPtr>       group_by;
    std::vector<OrderByItem>   order_by;
    std::optional<int64_t>     limit;
};

// ═══════════════════════════════════════════════════════════════════════
//  PARSER
// ═══════════════════════════════════════════════════════════════════════

/// Parse a SQL string into a SelectStmt AST.
/// @throws SqlError on syntax errors, unsupported constructs, etc.
SelectStmt parse_sql(const std::string& sql);

/// Convert an expression to a human-readable string (for EXPLAIN output).
std::string expr_to_string(const ExprPtr& expr);

}  // namespace snowquery
