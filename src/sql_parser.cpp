/// @file sql_parser.cpp
/// @brief Tokenizer and recursive-descent parser for SnowQuery SQL.

#include "snowquery/sql_parser.hpp"
#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_map>

namespace snowquery {

// ═══════════════════════════════════════════════════════════════════════
//  TOKENIZER
// ═══════════════════════════════════════════════════════════════════════

static const std::unordered_map<std::string, TokenType> keywords = {
    {"select", TokenType::Select}, {"from", TokenType::From},
    {"where", TokenType::Where},   {"and", TokenType::And},
    {"or", TokenType::Or},         {"not", TokenType::Not},
    {"order", TokenType::Order},   {"by", TokenType::By},
    {"asc", TokenType::Asc},       {"desc", TokenType::Desc},
    {"limit", TokenType::Limit},   {"group", TokenType::Group},
    {"count", TokenType::Count},   {"sum", TokenType::Sum},
    {"min", TokenType::Min},       {"max", TokenType::Max},
    {"as", TokenType::As},         {"on", TokenType::On},
    {"join", TokenType::Join},     {"inner", TokenType::Inner},
    {"left", TokenType::Left},     {"right", TokenType::Right},
    {"cross", TokenType::Cross},   {"explain", TokenType::Explain},
};

std::vector<Token> tokenize(const std::string& sql) {
    std::vector<Token> tokens;
    size_t i = 0;
    const size_t n = sql.size();

    while (i < n) {
        // Skip whitespace.
        if (std::isspace(static_cast<unsigned char>(sql[i]))) { ++i; continue; }

        size_t start = i;
        char ch = sql[i];

        // Single/double-char operators.
        if (ch == '(') { tokens.push_back({TokenType::LParen, "(", start}); ++i; continue; }
        if (ch == ')') { tokens.push_back({TokenType::RParen, ")", start}); ++i; continue; }
        if (ch == ',') { tokens.push_back({TokenType::Comma, ",", start}); ++i; continue; }
        if (ch == ';') { tokens.push_back({TokenType::Semicolon, ";", start}); ++i; continue; }
        if (ch == '*') { tokens.push_back({TokenType::Star, "*", start}); ++i; continue; }
        if (ch == '.') { tokens.push_back({TokenType::Dot, ".", start}); ++i; continue; }
        if (ch == '=') { tokens.push_back({TokenType::Eq, "=", start}); ++i; continue; }

        if (ch == '!' && i + 1 < n && sql[i + 1] == '=') {
            tokens.push_back({TokenType::Neq, "!=", start}); i += 2; continue;
        }
        if (ch == '<') {
            if (i + 1 < n && sql[i + 1] == '=') {
                tokens.push_back({TokenType::Le, "<=", start}); i += 2;
            } else if (i + 1 < n && sql[i + 1] == '>') {
                tokens.push_back({TokenType::Neq, "<>", start}); i += 2;
            } else {
                tokens.push_back({TokenType::Lt, "<", start}); ++i;
            }
            continue;
        }
        if (ch == '>') {
            if (i + 1 < n && sql[i + 1] == '=') {
                tokens.push_back({TokenType::Ge, ">=", start}); i += 2;
            } else {
                tokens.push_back({TokenType::Gt, ">", start}); ++i;
            }
            continue;
        }

        // String literal: 'text'.
        if (ch == '\'') {
            ++i;
            std::string lit;
            while (i < n) {
                if (sql[i] == '\'') {
                    if (i + 1 < n && sql[i + 1] == '\'') {
                        lit += '\''; i += 2;  // escaped single quote
                    } else {
                        ++i; break;
                    }
                } else {
                    lit += sql[i++];
                }
            }
            tokens.push_back({TokenType::StringLiteral, std::move(lit), start});
            continue;
        }

        // Numeric literal.
        if (std::isdigit(static_cast<unsigned char>(ch)) ||
            (ch == '-' && i + 1 < n && std::isdigit(static_cast<unsigned char>(sql[i + 1])) &&
             (tokens.empty() ||
              tokens.back().type == TokenType::LParen ||
              tokens.back().type == TokenType::Comma ||
              tokens.back().type == TokenType::Eq ||
              tokens.back().type == TokenType::Neq ||
              tokens.back().type == TokenType::Lt ||
              tokens.back().type == TokenType::Gt ||
              tokens.back().type == TokenType::Le ||
              tokens.back().type == TokenType::Ge ||
              tokens.back().type == TokenType::And ||
              tokens.back().type == TokenType::Or))) {
            bool has_dot = false;
            std::string num;
            if (ch == '-') { num += '-'; ++i; }
            while (i < n && (std::isdigit(static_cast<unsigned char>(sql[i])) || sql[i] == '.')) {
                if (sql[i] == '.') {
                    if (has_dot) break;
                    has_dot = true;
                }
                num += sql[i++];
            }
            tokens.push_back({has_dot ? TokenType::FloatLiteral : TokenType::IntLiteral,
                              std::move(num), start});
            continue;
        }

        // Identifier or keyword.
        if (std::isalpha(static_cast<unsigned char>(ch)) || ch == '_') {
            std::string word;
            while (i < n && (std::isalnum(static_cast<unsigned char>(sql[i])) || sql[i] == '_')) {
                word += sql[i++];
            }
            // Check if it's a keyword (case-insensitive).
            std::string lower = word;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            auto it = keywords.find(lower);
            if (it != keywords.end()) {
                tokens.push_back({it->second, std::move(word), start});
            } else {
                tokens.push_back({TokenType::Identifier, std::move(word), start});
            }
            continue;
        }

        throw SqlError("unexpected character '" + std::string(1, ch) +
                        "' at position " + std::to_string(i));
    }

    tokens.push_back({TokenType::Eof, "", sql.size()});
    return tokens;
}

// ═══════════════════════════════════════════════════════════════════════
//  AGGREGATE HELPERS
// ═══════════════════════════════════════════════════════════════════════

std::string agg_func_name(AggFunc f) {
    switch (f) {
        case AggFunc::Count: return "COUNT";
        case AggFunc::Sum:   return "SUM";
        case AggFunc::Min:   return "MIN";
        case AggFunc::Max:   return "MAX";
    }
    return "?";
}

// ═══════════════════════════════════════════════════════════════════════
//  PARSER — recursive descent
// ═══════════════════════════════════════════════════════════════════════

class Parser {
public:
    explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

    SelectStmt parse() {
        SelectStmt stmt;

        // Optional EXPLAIN prefix.
        if (peek().type == TokenType::Explain) {
            stmt.is_explain = true;
            advance();
        }

        expect(TokenType::Select, "expected SELECT");

        // SELECT list.
        stmt.select_list = parse_select_list();

        // FROM.
        expect(TokenType::From, "expected FROM");
        stmt.from_table = expect_identifier("expected table name after FROM");

        // Optional alias.
        if (peek().type == TokenType::Identifier || peek().type == TokenType::As) {
            if (peek().type == TokenType::As) advance();
            stmt.from_alias = expect_identifier("expected alias");
        }

        // JOINs.
        while (peek().type == TokenType::Join || peek().type == TokenType::Inner ||
               peek().type == TokenType::Left || peek().type == TokenType::Right ||
               peek().type == TokenType::Cross) {
            stmt.joins.push_back(parse_join());
        }

        // WHERE.
        if (peek().type == TokenType::Where) {
            advance();
            stmt.where_clause = parse_or_expr();
        }

        // GROUP BY.
        if (peek().type == TokenType::Group) {
            advance();
            expect(TokenType::By, "expected BY after GROUP");
            stmt.group_by = parse_expr_list();
        }

        // ORDER BY.
        if (peek().type == TokenType::Order) {
            advance();
            expect(TokenType::By, "expected BY after ORDER");
            stmt.order_by = parse_order_list();
        }

        // LIMIT.
        if (peek().type == TokenType::Limit) {
            advance();
            const auto& tok = peek();
            if (tok.type != TokenType::IntLiteral)
                throw SqlError("expected integer after LIMIT");
            stmt.limit = std::stoll(tok.text);
            advance();
        }

        // Optional trailing semicolon.
        if (peek().type == TokenType::Semicolon) advance();

        if (peek().type != TokenType::Eof)
            throw SqlError("unexpected token after statement: " + peek().text);

        return stmt;
    }

private:
    std::vector<Token> tokens_;
    size_t pos_ = 0;

    const Token& peek() const { return tokens_[pos_]; }
    const Token& advance() { return tokens_[pos_++]; }

    void expect(TokenType t, const std::string& msg) {
        if (peek().type != t) throw SqlError(msg + " (got '" + peek().text + "')");
        advance();
    }

    std::string expect_identifier(const std::string& msg) {
        if (peek().type != TokenType::Identifier)
            throw SqlError(msg + " (got '" + peek().text + "')");
        std::string s = peek().text;
        advance();
        return s;
    }

    // ── SELECT list ──────────────────────────────────────────────
    std::vector<SelectItem> parse_select_list() {
        std::vector<SelectItem> items;
        items.push_back(parse_select_item());
        while (peek().type == TokenType::Comma) {
            advance();
            items.push_back(parse_select_item());
        }
        return items;
    }

    SelectItem parse_select_item() {
        SelectItem item;
        item.expr = parse_select_expr();
        if (peek().type == TokenType::As) {
            advance();
            item.alias = expect_identifier("expected alias after AS");
        } else if (peek().type == TokenType::Identifier &&
                   peek().text != "FROM" && peek().text != "from") {
            // Implicit alias (without AS keyword) — but only if it
            // doesn't collide with a keyword.
            auto lower = peek().text;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (keywords.find(lower) == keywords.end()) {
                item.alias = peek().text;
                advance();
            }
        }
        return item;
    }

    ExprPtr parse_select_expr() {
        // Aggregate functions.
        if (is_agg_keyword(peek().type)) {
            return parse_agg_expr();
        }
        // Star.
        if (peek().type == TokenType::Star) {
            advance();
            return std::make_shared<StarExpr>();
        }
        // Column ref (possibly table.column).
        return parse_column_or_literal();
    }

    bool is_agg_keyword(TokenType t) const {
        return t == TokenType::Count || t == TokenType::Sum ||
               t == TokenType::Min || t == TokenType::Max;
    }

    ExprPtr parse_agg_expr() {
        AggFunc func;
        switch (peek().type) {
            case TokenType::Count: func = AggFunc::Count; break;
            case TokenType::Sum:   func = AggFunc::Sum;   break;
            case TokenType::Min:   func = AggFunc::Min;   break;
            case TokenType::Max:   func = AggFunc::Max;   break;
            default: throw SqlError("internal: not an aggregate");
        }
        advance();
        expect(TokenType::LParen, "expected '(' after aggregate function");
        ExprPtr arg;
        if (peek().type == TokenType::Star) {
            arg = std::make_shared<StarExpr>();
            advance();
        } else {
            arg = parse_column_or_literal();
        }
        expect(TokenType::RParen, "expected ')' after aggregate argument");
        auto agg = std::make_shared<AggExpr>();
        agg->func = func;
        agg->arg = std::move(arg);
        return agg;
    }

    // ── Expressions (WHERE clause) ──────────────────────────────
    // Precedence: OR < AND < comparison < atom.

    ExprPtr parse_or_expr() {
        auto left = parse_and_expr();
        while (peek().type == TokenType::Or) {
            advance();
            auto right = parse_and_expr();
            auto bin = std::make_shared<BinaryExpr>();
            bin->op = BinaryExpr::Or;
            bin->left = std::move(left);
            bin->right = std::move(right);
            left = bin;
        }
        return left;
    }

    ExprPtr parse_and_expr() {
        auto left = parse_comparison();
        while (peek().type == TokenType::And) {
            advance();
            auto right = parse_comparison();
            auto bin = std::make_shared<BinaryExpr>();
            bin->op = BinaryExpr::And;
            bin->left = std::move(left);
            bin->right = std::move(right);
            left = bin;
        }
        return left;
    }

    ExprPtr parse_comparison() {
        auto left = parse_atom();
        if (is_comparison_op(peek().type)) {
            BinaryExpr::Op op;
            switch (peek().type) {
                case TokenType::Eq:  op = BinaryExpr::Eq; break;
                case TokenType::Neq: op = BinaryExpr::Neq; break;
                case TokenType::Lt:  op = BinaryExpr::Lt; break;
                case TokenType::Gt:  op = BinaryExpr::Gt; break;
                case TokenType::Le:  op = BinaryExpr::Le; break;
                case TokenType::Ge:  op = BinaryExpr::Ge; break;
                default: throw SqlError("internal: unknown comparison op");
            }
            advance();
            auto right = parse_atom();
            auto bin = std::make_shared<BinaryExpr>();
            bin->op = op;
            bin->left = std::move(left);
            bin->right = std::move(right);
            return bin;
        }
        return left;
    }

    bool is_comparison_op(TokenType t) const {
        return t == TokenType::Eq || t == TokenType::Neq ||
               t == TokenType::Lt || t == TokenType::Gt ||
               t == TokenType::Le || t == TokenType::Ge;
    }

    ExprPtr parse_atom() {
        if (peek().type == TokenType::LParen) {
            advance();
            auto expr = parse_or_expr();
            expect(TokenType::RParen, "expected ')'");
            return expr;
        }
        return parse_column_or_literal();
    }

    ExprPtr parse_column_or_literal() {
        const auto& tok = peek();
        if (tok.type == TokenType::IntLiteral) {
            auto lit = std::make_shared<IntLit>();
            lit->value = std::stoll(tok.text);
            advance();
            return lit;
        }
        if (tok.type == TokenType::FloatLiteral) {
            auto lit = std::make_shared<FloatLit>();
            lit->value = std::stod(tok.text);
            advance();
            return lit;
        }
        if (tok.type == TokenType::StringLiteral) {
            auto lit = std::make_shared<StringLit>();
            lit->value = tok.text;
            advance();
            return lit;
        }
        if (tok.type == TokenType::Identifier) {
            auto col = std::make_shared<ColumnRef>();
            col->column = tok.text;
            advance();
            // Check for table.column syntax.
            if (peek().type == TokenType::Dot) {
                advance();
                col->table = col->column;
                col->column = expect_identifier("expected column name after '.'");
            }
            return col;
        }
        // Aggregate in WHERE/ORDER BY context.
        if (is_agg_keyword(tok.type)) {
            return parse_agg_expr();
        }
        throw SqlError("unexpected token: '" + tok.text + "' at position " +
                        std::to_string(tok.pos));
    }

    // ── Expression lists ─────────────────────────────────────────
    std::vector<ExprPtr> parse_expr_list() {
        std::vector<ExprPtr> list;
        list.push_back(parse_column_or_literal());
        while (peek().type == TokenType::Comma) {
            advance();
            list.push_back(parse_column_or_literal());
        }
        return list;
    }

    std::vector<OrderByItem> parse_order_list() {
        std::vector<OrderByItem> list;
        list.push_back(parse_order_item());
        while (peek().type == TokenType::Comma) {
            advance();
            list.push_back(parse_order_item());
        }
        return list;
    }

    OrderByItem parse_order_item() {
        OrderByItem item;
        if (is_agg_keyword(peek().type)) {
            item.expr = parse_agg_expr();
        } else {
            item.expr = parse_column_or_literal();
        }
        if (peek().type == TokenType::Asc) { item.dir = SortDir::Asc; advance(); }
        else if (peek().type == TokenType::Desc) { item.dir = SortDir::Desc; advance(); }
        return item;
    }

    // ── JOIN ─────────────────────────────────────────────────────
    JoinClause parse_join() {
        // Skip optional INNER/LEFT/RIGHT/CROSS keyword.
        if (peek().type == TokenType::Inner || peek().type == TokenType::Left ||
            peek().type == TokenType::Right || peek().type == TokenType::Cross) {
            advance();
        }
        expect(TokenType::Join, "expected JOIN");
        JoinClause jc;
        jc.table_name = expect_identifier("expected table name after JOIN");
        // Optional alias.
        if (peek().type == TokenType::As) {
            advance();
            jc.alias = expect_identifier("expected alias after AS");
        } else if (peek().type == TokenType::Identifier) {
            auto lower = peek().text;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (lower != "on" && keywords.find(lower) == keywords.end()) {
                jc.alias = peek().text;
                advance();
            }
        }
        if (peek().type == TokenType::On) {
            advance();
            jc.on_condition = parse_or_expr();
        }
        return jc;
    }
};

SelectStmt parse_sql(const std::string& sql) {
    auto tokens = tokenize(sql);
    Parser parser(std::move(tokens));
    return parser.parse();
}

// ═══════════════════════════════════════════════════════════════════════
//  EXPRESSION → STRING (for EXPLAIN)
// ═══════════════════════════════════════════════════════════════════════

std::string expr_to_string(const ExprPtr& expr) {
    if (!expr) return "<null>";
    if (auto* c = dynamic_cast<ColumnRef*>(expr.get())) {
        if (!c->table.empty()) return c->table + "." + c->column;
        return c->column;
    }
    if (auto* i = dynamic_cast<IntLit*>(expr.get()))
        return std::to_string(i->value);
    if (auto* f = dynamic_cast<FloatLit*>(expr.get())) {
        std::ostringstream os;
        os << f->value;
        return os.str();
    }
    if (auto* s = dynamic_cast<StringLit*>(expr.get()))
        return "'" + s->value + "'";
    if (dynamic_cast<StarExpr*>(expr.get()))
        return "*";
    if (auto* a = dynamic_cast<AggExpr*>(expr.get()))
        return agg_func_name(a->func) + "(" + expr_to_string(a->arg) + ")";
    if (auto* b = dynamic_cast<BinaryExpr*>(expr.get())) {
        static const char* op_strs[] = {"=","!=","<",">","<=",">=","AND","OR"};
        return "(" + expr_to_string(b->left) + " " +
               op_strs[static_cast<int>(b->op)] + " " +
               expr_to_string(b->right) + ")";
    }
    return "<?>";
}

}  // namespace snowquery
