#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "flatfiledb/domain/ast.h"
#include "flatfiledb/domain/value.h"
#include "flatfiledb/sql_parser.h"
#include "test_harness.h"

namespace {

namespace adapter = flatfiledb::adapter;
namespace domain = flatfiledb::domain;

constexpr int64_t kInt64Max = std::numeric_limits<int64_t>::max();
constexpr int64_t kInt64Min = std::numeric_limits<int64_t>::min();

// Mirrors kMaxExpressionDepth in src/sql_parser.cpp: the deepest expression
// tree the parser accepts. A left-leaning chain of n operators has depth n + 1.
constexpr std::size_t kParserMaxExpressionDepth = 500;

void ReportError(const std::string& message) {
  flatfiledb::testing::ReportFailure(__FILE__, __LINE__, message);
}

// Rendering of the AST for shape assertions. A binary node reads OP(lhs, rhs)
// with OP in EQ NE LT LE GT GE AND OR ADD SUB MUL DIV CONCAT LIKE; a function
// reads NAME(arg, ...) and CAST(arg AS TYPE); an INT literal is printed
// plainly, a DOUBLE with a trailing "d", a TEXT literal in single quotes.

const char* OperatorName(domain::BinaryOp op) {
  switch (op) {
    case domain::BinaryOp::kEq:
      return "EQ";
    case domain::BinaryOp::kNe:
      return "NE";
    case domain::BinaryOp::kLt:
      return "LT";
    case domain::BinaryOp::kLe:
      return "LE";
    case domain::BinaryOp::kGt:
      return "GT";
    case domain::BinaryOp::kGe:
      return "GE";
    case domain::BinaryOp::kAnd:
      return "AND";
    case domain::BinaryOp::kOr:
      return "OR";
    case domain::BinaryOp::kAdd:
      return "ADD";
    case domain::BinaryOp::kSub:
      return "SUB";
    case domain::BinaryOp::kMul:
      return "MUL";
    case domain::BinaryOp::kDiv:
      return "DIV";
    case domain::BinaryOp::kConcat:
      return "CONCAT";
    case domain::BinaryOp::kLike:
      return "LIKE";
  }
  return "?";
}

const char* AggregateName(domain::AggregateFn fn) {
  switch (fn) {
    case domain::AggregateFn::kCount:
      return "COUNT";
    case domain::AggregateFn::kSum:
      return "SUM";
    case domain::AggregateFn::kAvg:
      return "AVG";
    case domain::AggregateFn::kMin:
      return "MIN";
    case domain::AggregateFn::kMax:
      break;
  }
  return "MAX";
}

const char* FunctionName(domain::ScalarFn fn) {
  switch (fn) {
    case domain::ScalarFn::kLower:
      return "LOWER";
    case domain::ScalarFn::kUpper:
      return "UPPER";
    case domain::ScalarFn::kLength:
      return "LENGTH";
    case domain::ScalarFn::kSubstr:
      return "SUBSTR";
    case domain::ScalarFn::kTrim:
      return "TRIM";
    case domain::ScalarFn::kAbs:
      return "ABS";
    case domain::ScalarFn::kRound:
      return "ROUND";
    case domain::ScalarFn::kCast:
      break;
  }
  return "CAST";
}

const char* TypeKeyword(domain::DataType type) {
  switch (type) {
    case domain::DataType::kInt:
      return "INT";
    case domain::DataType::kDouble:
      return "DOUBLE";
    case domain::DataType::kText:
      break;
  }
  return "TEXT";
}

std::string DescribeValue(const domain::Value& value) {
  if (const auto* integer = std::get_if<int64_t>(&value)) {
    return std::to_string(*integer);
  }
  if (const auto* text = std::get_if<std::string>(&value)) {
    return "'" + *text + "'";
  }
  std::ostringstream stream;
  stream << std::setprecision(17) << std::get<double>(value) << "d";
  return stream.str();
}

std::string DescribeExpr(const domain::Expr* expr) {
  if (expr == nullptr) {
    return "<null>";
  }
  if (const auto* literal = std::get_if<domain::Literal>(&expr->node)) {
    return DescribeValue(literal->value);
  }
  if (const auto* column = std::get_if<domain::ColumnRef>(&expr->node)) {
    return column->qualifier.empty() ? column->name
                                     : column->qualifier + "." + column->name;
  }
  if (const auto* binary = std::get_if<domain::BinaryExpr>(&expr->node)) {
    return std::string(OperatorName(binary->op)) + "(" +
           DescribeExpr(binary->lhs.get()) + ", " +
           DescribeExpr(binary->rhs.get()) + ")";
  }
  if (const auto* function = std::get_if<domain::FunctionCall>(&expr->node)) {
    std::string text = FunctionName(function->fn);
    text += "(";
    for (std::size_t i = 0; i < function->args.size(); ++i) {
      if (i > 0) {
        text += ", ";
      }
      text += DescribeExpr(function->args[i].get());
    }
    if (function->fn == domain::ScalarFn::kCast) {
      text += " AS " + std::string(TypeKeyword(function->cast_type));
    }
    return text + ")";
  }
  const auto& aggregate = std::get<domain::AggregateCall>(expr->node);
  const std::string arg =
      aggregate.arg ? DescribeExpr(aggregate.arg.get()) : "*";
  return std::string(AggregateName(aggregate.fn)) + "(" + arg + ")";
}

const domain::Value* LiteralValue(const domain::Expr* expr) {
  if (expr == nullptr) {
    return nullptr;
  }
  const auto* literal = std::get_if<domain::Literal>(&expr->node);
  return literal == nullptr ? nullptr : &literal->value;
}

const domain::AggregateCall* AsAggregate(const domain::Expr* expr) {
  return expr == nullptr ? nullptr
                         : std::get_if<domain::AggregateCall>(&expr->node);
}

// Parsing helpers. They report a test failure when the parse result is not the
// expected kind and then return an empty value, so later checks fail too
// without dereferencing anything.

template <typename StatementType>
StatementType ParseAs(const std::string& sql) {
  adapter::ParseResult result = adapter::ParseStatement(sql);
  if (const auto* error = std::get_if<adapter::ParseError>(&result)) {
    ReportError("parse failed for [" + sql + "] at column " +
                std::to_string(error->column) + ": " + error->description);
    return StatementType{};
  }
  auto* statement = std::get_if<domain::Statement>(&result);
  auto* typed = std::get_if<StatementType>(statement);
  if (typed == nullptr) {
    ReportError("unexpected statement kind for [" + sql + "]");
    return StatementType{};
  }
  return std::move(*typed);
}

adapter::ParseError ParseFailure(const std::string& sql) {
  adapter::ParseResult result = adapter::ParseStatement(sql);
  if (const auto* error = std::get_if<adapter::ParseError>(&result)) {
    return *error;
  }
  ReportError("expected a ParseError for [" + sql + "]");
  return adapter::ParseError{0, ""};
}

// Parses `SELECT <expression_text> FROM t` and returns the only item's
// expression, so expression tests do not depend on a statement clause.
domain::ExprPtr ParseExpressionText(const std::string& expression_text) {
  domain::SelectStatement select =
      ParseAs<domain::SelectStatement>("SELECT " + expression_text + " FROM t");
  if (select.items.size() != 1) {
    ReportError("expected one select item for [" + expression_text + "]");
    return nullptr;
  }
  return std::move(select.items[0].expr);
}

void CheckShape(const std::string& expression_text,
                const std::string& expected_shape) {
  domain::ExprPtr expr = ParseExpressionText(expression_text);
  const std::string actual_shape = DescribeExpr(expr.get());
  if (actual_shape != expected_shape) {
    ReportError("[" + expression_text + "] parsed as " + actual_shape +
                ", expected " + expected_shape);
  }
}

// The error has the expected column and a non-empty description.
void CheckErrorAt(const std::string& sql, std::size_t expected_column) {
  const adapter::ParseError error = ParseFailure(sql);
  if (error.column != expected_column) {
    ReportError("[" + sql + "] failed at column " +
                std::to_string(error.column) + ", expected " +
                std::to_string(expected_column) + " (" + error.description +
                ")");
  }
  if (error.description.empty()) {
    ReportError("[" + sql + "] has an empty description");
  }
}

bool Parses(const std::string& sql) {
  return std::holds_alternative<domain::Statement>(
      adapter::ParseStatement(sql));
}

// `SELECT 1 - 1 - ... - 1 FROM t` with operator_count subtractions.
std::string SubtractionChain(std::size_t operator_count) {
  std::string sql = "SELECT 1";
  for (std::size_t i = 0; i < operator_count; ++i) {
    sql += " - 1";
  }
  return sql + " FROM t";
}

// Returns the item at index, or null after reporting a failure.
const domain::SelectItem* ItemAt(const domain::SelectStatement& select,
                                 std::size_t index) {
  if (index < select.items.size()) {
    return &select.items[index];
  }
  ReportError("select item " + std::to_string(index) + " is missing");
  return nullptr;
}

struct ErrorCase {
  const char* sql;
  std::size_t column;
};

struct ShapeCase {
  const char* text;
  const char* shape;
};

}  // namespace

TEST(test_parser_select_star_builds_ast) {
  domain::SelectStatement select =
      ParseAs<domain::SelectStatement>("SELECT * FROM users");

  CHECK_EQ(select.items.size(), 1u);
  const domain::SelectItem* item = ItemAt(select, 0);
  if (item != nullptr) {
    CHECK(item->is_star);
    CHECK(item->expr == nullptr);
  }
  CHECK_EQ(select.from.table, "users");
  CHECK(select.from.alias.empty());
  CHECK(select.joins.empty());
  CHECK(select.where == nullptr);
  CHECK(select.group_by.empty());
  CHECK(select.order_by.empty());
}

TEST(test_parser_select_full_clauses_builds_ast) {
  domain::SelectStatement full = ParseAs<domain::SelectStatement>(
      "SELECT u.username AS name, COUNT(*) FROM users AS u "
      "JOIN posts p ON u.username = p.username WHERE u.id > 1 "
      "GROUP BY u.username ORDER BY name DESC");
  domain::SelectStatement ascending =
      ParseAs<domain::SelectStatement>("SELECT id FROM users ORDER BY id ASC");

  CHECK_EQ(full.items.size(), 2u);
  const domain::SelectItem* aliased_item = ItemAt(full, 0);
  if (aliased_item != nullptr) {
    CHECK_EQ(aliased_item->alias, "name");
    CHECK_EQ(DescribeExpr(aliased_item->expr.get()), "u.username");
  }
  const domain::SelectItem* count_item = ItemAt(full, 1);
  if (count_item != nullptr) {
    CHECK_EQ(DescribeExpr(count_item->expr.get()), "COUNT(*)");
  }
  CHECK_EQ(full.from.table, "users");
  CHECK_EQ(full.from.alias, "u");
  CHECK_EQ(full.joins.size(), 1u);
  if (full.joins.size() == 1) {
    CHECK_EQ(full.joins[0].table.table, "posts");
    CHECK_EQ(full.joins[0].table.alias, "p");
    CHECK_EQ(DescribeExpr(full.joins[0].on.get()),
             "EQ(u.username, p.username)");
  }
  CHECK(full.where != nullptr);
  CHECK_EQ(DescribeExpr(full.where.get()), "GT(u.id, 1)");
  CHECK_EQ(full.group_by.size(), 1u);
  if (full.group_by.size() == 1) {
    CHECK_EQ(DescribeExpr(full.group_by[0].get()), "u.username");
  }
  CHECK_EQ(full.order_by.size(), 1u);
  if (full.order_by.size() == 1) {
    CHECK(full.order_by[0].is_descending);
    CHECK_EQ(DescribeExpr(full.order_by[0].expr.get()), "name");
  }

  CHECK_EQ(ascending.order_by.size(), 1u);
  if (ascending.order_by.size() == 1) {
    CHECK(!ascending.order_by[0].is_descending);
  }
}

TEST(test_parser_expression_precedence_orders_operators) {
  const ShapeCase cases[] = {
      {"a OR b AND c", "OR(a, AND(b, c))"},
      {"1 - 2 - 3", "SUB(SUB(1, 2), 3)"},
      {"a = 1 + 2", "EQ(a, ADD(1, 2))"},
  };

  for (const ShapeCase& shape_case : cases) {
    CheckShape(shape_case.text, shape_case.shape);
  }
}

TEST(test_parser_keywords_are_case_insensitive) {
  domain::SelectStatement select =
      ParseAs<domain::SelectStatement>("select ID from USERS where Id = 1");

  CHECK_EQ(select.from.table, "USERS");
  const domain::SelectItem* item = ItemAt(select, 0);
  if (item != nullptr) {
    CHECK_EQ(DescribeExpr(item->expr.get()), "ID");
    CHECK_EQ(item->text, "ID");
  }
  CHECK_EQ(DescribeExpr(select.where.get()), "EQ(Id, 1)");
}

TEST(test_parser_string_literal_handles_doubled_quote) {
  domain::ExprPtr escaped = ParseExpressionText("'it''s'");
  domain::ExprPtr empty = ParseExpressionText("''");

  const domain::Value* escaped_value = LiteralValue(escaped.get());
  CHECK(escaped_value != nullptr);
  if (escaped_value != nullptr) {
    CHECK(std::holds_alternative<std::string>(*escaped_value));
    CHECK_EQ(DescribeValue(*escaped_value), "'it's'");
  }
  const domain::Value* empty_value = LiteralValue(empty.get());
  CHECK(empty_value != nullptr);
  if (empty_value != nullptr) {
    CHECK(std::holds_alternative<std::string>(*empty_value));
    CHECK_EQ(DescribeValue(*empty_value), "''");
  }
}

TEST(test_parser_numeric_literals_keep_type) {
  domain::ExprPtr zero = ParseExpressionText("0");
  domain::ExprPtr pi = ParseExpressionText("3.14");
  domain::ExprPtr largest = ParseExpressionText("9223372036854775807");

  const domain::Value* zero_value = LiteralValue(zero.get());
  CHECK(zero_value != nullptr && std::holds_alternative<int64_t>(*zero_value));
  if (zero_value != nullptr && std::holds_alternative<int64_t>(*zero_value)) {
    CHECK_EQ(std::get<int64_t>(*zero_value), int64_t{0});
  }
  const domain::Value* pi_value = LiteralValue(pi.get());
  CHECK(pi_value != nullptr && std::holds_alternative<double>(*pi_value));
  if (pi_value != nullptr && std::holds_alternative<double>(*pi_value)) {
    CHECK_EQ(std::get<double>(*pi_value), 3.14);
  }
  const domain::Value* largest_value = LiteralValue(largest.get());
  CHECK(largest_value != nullptr &&
        std::holds_alternative<int64_t>(*largest_value));
  if (largest_value != nullptr &&
      std::holds_alternative<int64_t>(*largest_value)) {
    CHECK_EQ(std::get<int64_t>(*largest_value), kInt64Max);
  }
}

TEST(test_parser_negative_literals_and_lowering) {
  const ShapeCase cases[] = {
      {"-5", "-5"},
      {"-9223372036854775808", "-9223372036854775808"},
      {"-3.5", "-3.5d"},
      {"-views", "SUB(0, views)"},
      {"1 - -2", "SUB(1, -2)"},
      {"1-2", "SUB(1, 2)"},
  };

  for (const ShapeCase& shape_case : cases) {
    CheckShape(shape_case.text, shape_case.shape);
  }

  domain::ExprPtr smallest = ParseExpressionText("-9223372036854775808");
  const domain::Value* smallest_value = LiteralValue(smallest.get());
  CHECK(smallest_value != nullptr &&
        std::holds_alternative<int64_t>(*smallest_value));
  if (smallest_value != nullptr &&
      std::holds_alternative<int64_t>(*smallest_value)) {
    CHECK_EQ(std::get<int64_t>(*smallest_value), kInt64Min);
  }
}

TEST(test_parser_insert_with_and_without_columns) {
  domain::InsertStatement with_columns = ParseAs<domain::InsertStatement>(
      "INSERT INTO users (id, username, location) VALUES (9, 'zed', 'Oslo')");
  domain::InsertStatement without_columns = ParseAs<domain::InsertStatement>(
      "INSERT INTO users VALUES (9, 'zed', 'Oslo')");

  CHECK_EQ(with_columns.table, "users");
  const std::vector<std::string> expected_columns = {"id", "username",
                                                     "location"};
  CHECK(with_columns.columns == expected_columns);
  CHECK(without_columns.columns.empty());
  CHECK_EQ(with_columns.rows.size(), 1u);
  CHECK_EQ(without_columns.rows.size(), 1u);
  if (with_columns.rows.size() == 1 && with_columns.rows[0].size() == 3) {
    CHECK_EQ(without_columns.rows[0].size(), 3u);
    CHECK_EQ(DescribeExpr(with_columns.rows[0][0].get()), "9");
    CHECK_EQ(DescribeExpr(with_columns.rows[0][1].get()), "'zed'");
    CHECK_EQ(DescribeExpr(with_columns.rows[0][2].get()), "'Oslo'");
  }
}

TEST(test_parser_update_assignments_and_optional_where) {
  domain::UpdateStatement with_where = ParseAs<domain::UpdateStatement>(
      "UPDATE users SET username = 'zed', location = 'Oslo' WHERE id = 1");
  domain::UpdateStatement without_where =
      ParseAs<domain::UpdateStatement>("UPDATE users SET location = 'Oslo'");

  CHECK_EQ(with_where.table, "users");
  CHECK_EQ(with_where.assignments.size(), 2u);
  if (with_where.assignments.size() == 2) {
    CHECK_EQ(with_where.assignments[0].column, "username");
    CHECK_EQ(DescribeExpr(with_where.assignments[0].value.get()), "'zed'");
    CHECK_EQ(with_where.assignments[1].column, "location");
    CHECK_EQ(DescribeExpr(with_where.assignments[1].value.get()), "'Oslo'");
  }
  CHECK(with_where.where != nullptr);
  CHECK_EQ(DescribeExpr(with_where.where.get()), "EQ(id, 1)");
  CHECK_EQ(without_where.assignments.size(), 1u);
  CHECK(without_where.where == nullptr);
}

TEST(test_parser_delete_optional_where) {
  domain::DeleteStatement with_where =
      ParseAs<domain::DeleteStatement>("DELETE FROM users WHERE id = 1");
  domain::DeleteStatement without_where =
      ParseAs<domain::DeleteStatement>("DELETE FROM users");

  CHECK_EQ(with_where.table, "users");
  CHECK(with_where.where != nullptr);
  CHECK_EQ(DescribeExpr(with_where.where.get()), "EQ(id, 1)");
  CHECK_EQ(without_where.table, "users");
  CHECK(without_where.where == nullptr);
}

TEST(test_parser_trailing_semicolon_rules) {
  domain::DeleteStatement with_semicolon =
      ParseAs<domain::DeleteStatement>("DELETE FROM t;");
  domain::DeleteStatement without_semicolon =
      ParseAs<domain::DeleteStatement>("DELETE FROM t");

  CHECK_EQ(with_semicolon.table, "t");
  CHECK_EQ(without_semicolon.table, "t");
  CheckErrorAt("DELETE FROM t; x", 16);
}

TEST(test_parser_invalid_input_reports_column) {
  const ErrorCase cases[] = {
      {"SELEC username FROM users;", 1},
      {"SELECT FROM users", 8},
      {"SELECT 'abc FROM users", 8},
      {"SELECT 1 # 2 FROM t", 10},
      {"SELECT 9223372036854775808 FROM t", 8},
      {"SELECT id FROM", 15},
      {"SELECT 1e3 FROM t", 9},
  };

  for (const ErrorCase& error_case : cases) {
    CheckErrorAt(error_case.sql, error_case.column);
  }
  CHECK_EQ(ParseFailure("SELEC username FROM users;").description,
           "unrecognized statement");
}

TEST(test_parser_select_items_keep_alias_and_source_text) {
  domain::SelectStatement select = ParseAs<domain::SelectStatement>(
      "SELECT COUNT(*), username AS name, id + 1 FROM users");

  CHECK_EQ(select.items.size(), 3u);
  const domain::SelectItem* count_item = ItemAt(select, 0);
  const domain::SelectItem* name_item = ItemAt(select, 1);
  const domain::SelectItem* sum_item = ItemAt(select, 2);
  if (count_item == nullptr || name_item == nullptr || sum_item == nullptr) {
    return;
  }
  CHECK_EQ(count_item->text, "COUNT(*)");
  CHECK_EQ(name_item->text, "username");
  CHECK_EQ(sum_item->text, "id + 1");
  CHECK(count_item->alias.empty());
  CHECK_EQ(name_item->alias, "name");
  CHECK(sum_item->alias.empty());
  CHECK(!count_item->is_star && !name_item->is_star && !sum_item->is_star);
}

TEST(test_parser_aggregate_calls_build_ast) {
  domain::SelectStatement select = ParseAs<domain::SelectStatement>(
      "SELECT COUNT(*), COUNT(views), SUM(views + 1) FROM posts");

  CHECK_EQ(select.items.size(), 3u);
  const domain::SelectItem* count_star = ItemAt(select, 0);
  const domain::SelectItem* count_column = ItemAt(select, 1);
  const domain::SelectItem* sum_item = ItemAt(select, 2);
  if (count_star == nullptr || count_column == nullptr || sum_item == nullptr) {
    return;
  }
  const domain::AggregateCall* count_star_call =
      AsAggregate(count_star->expr.get());
  const domain::AggregateCall* count_column_call =
      AsAggregate(count_column->expr.get());
  const domain::AggregateCall* sum_call = AsAggregate(sum_item->expr.get());
  CHECK(count_star_call != nullptr && count_column_call != nullptr &&
        sum_call != nullptr);
  if (count_star_call == nullptr || count_column_call == nullptr ||
      sum_call == nullptr) {
    return;
  }
  CHECK(count_star_call->fn == domain::AggregateFn::kCount);
  CHECK(count_column_call->fn == domain::AggregateFn::kCount);
  CHECK(sum_call->fn == domain::AggregateFn::kSum);
  CHECK(count_star_call->arg == nullptr);
  CHECK(count_column_call->arg != nullptr);
  CHECK(sum_call->arg != nullptr);
  CHECK_EQ(DescribeExpr(count_column_call->arg.get()), "views");
  CHECK_EQ(DescribeExpr(sum_call->arg.get()), "ADD(views, 1)");
}

TEST(test_parser_unsupported_syntax_returns_parse_error) {
  const char* const statements[] = {
      "SELECT id FROM t WHERE NOT a = 1",
      "SELECT id FROM t WHERE a NOT LIKE 'x'",
      "SELECT id FROM t WHERE a IN (1, 2)",
      "SELECT id FROM t WHERE a IN (SELECT 1)",
      "CREATE INDEX i ON t (a)",
      "TRUNCATE TABLE t",
  };

  for (const char* sql : statements) {
    const adapter::ParseError error = ParseFailure(sql);
    CHECK(error.column >= 1);
    CHECK(!error.description.empty());
  }
}

TEST(test_parser_null_is_rejected) {
  const adapter::ParseError in_values =
      ParseFailure("INSERT INTO t VALUES (NULL)");
  const adapter::ParseError in_comparison =
      ParseFailure("SELECT id FROM t WHERE a = NULL");

  CHECK_EQ(in_values.description, "NULL is not supported");
  CHECK_EQ(in_values.column, 23u);
  CHECK_EQ(in_comparison.description, "NULL is not supported");
  CHECK_EQ(in_comparison.column, 28u);
}

TEST(test_parser_dot_commands_are_not_sql) {
  const adapter::ParseError open_command = ParseFailure(".open a b c");
  const adapter::ParseError unknown_command = ParseFailure(".foo");

  CHECK_EQ(open_command.column, 1u);
  CHECK(!open_command.description.empty());
  CHECK_EQ(unknown_command.column, 1u);
  CHECK(!unknown_command.description.empty());
}

TEST(test_parser_comparison_operators_and_parentheses) {
  const ShapeCase cases[] = {
      {"(a OR b) AND c", "AND(OR(a, b), c)"},
      {"a <> 1", "NE(a, 1)"},
      {"a != 1", "NE(a, 1)"},
      {"a <= 1", "LE(a, 1)"},
      {"a >= 1", "GE(a, 1)"},
  };

  for (const ShapeCase& shape_case : cases) {
    CheckShape(shape_case.text, shape_case.shape);
  }
}

// Grammar details of docs/DesignDocument.md section 2.3 that the cases above
// do not cover.

TEST(test_parser_chained_comparison_returns_parse_error) {
  CheckErrorAt("SELECT a FROM t WHERE a = b = c", 29);
  CheckErrorAt("SELECT a FROM t WHERE a < b > c", 29);
  CheckShape("(a = b) = c", "EQ(EQ(a, b), c)");
}

TEST(test_parser_integer_literal_bounds_report_literal_start) {
  CheckShape("9223372036854775807", "9223372036854775807");
  CheckShape("-9223372036854775808", "-9223372036854775808");
  CheckErrorAt("SELECT 9223372036854775808 FROM t", 8);
  CheckErrorAt("SELECT -9223372036854775809 FROM t", 8);
  CheckErrorAt("SELECT 1 - 9223372036854775808 FROM t", 12);
  CheckErrorAt("SELECT 99999999999999999999 FROM t", 8);
}

TEST(test_parser_minus_before_blank_lowers_to_subtraction) {
  CheckShape("- 5", "SUB(0, 5)");
  CheckShape("-(1 + 2)", "SUB(0, ADD(1, 2))");
  CheckShape("a -5", "SUB(a, 5)");
  CheckShape("1 - - 2", "SUB(1, SUB(0, 2))");
  CheckShape("- -5", "SUB(0, -5)");
}

TEST(test_parser_select_item_text_keeps_written_spelling) {
  domain::SelectStatement select = ParseAs<domain::SelectStatement>(
      "SELECT  (a+1) , Count( * )  AS  total ,\tu . id  FROM t");

  CHECK_EQ(select.items.size(), 3u);
  const domain::SelectItem* parenthesized = ItemAt(select, 0);
  const domain::SelectItem* count_item = ItemAt(select, 1);
  const domain::SelectItem* qualified = ItemAt(select, 2);
  if (parenthesized == nullptr || count_item == nullptr ||
      qualified == nullptr) {
    return;
  }
  CHECK_EQ(parenthesized->text, "(a+1)");
  CHECK_EQ(count_item->text, "Count( * )");
  CHECK_EQ(count_item->alias, "total");
  CHECK_EQ(qualified->text, "u . id");
}

TEST(test_parser_table_alias_forms_and_join_type_words) {
  domain::SelectStatement implicit_alias =
      ParseAs<domain::SelectStatement>("SELECT 1 FROM users Usr WHERE 1 = 1");
  domain::SelectStatement explicit_alias =
      ParseAs<domain::SelectStatement>("SELECT 1 FROM users AS Usr");
  domain::SelectStatement no_alias =
      ParseAs<domain::SelectStatement>("SELECT 1 FROM users WHERE 1 = 1");

  CHECK_EQ(implicit_alias.from.alias, "Usr");
  CHECK_EQ(explicit_alias.from.alias, "Usr");
  CHECK(no_alias.from.alias.empty());
  CheckErrorAt("SELECT 1 FROM users AS", 23);
  CheckErrorAt("SELECT 1 FROM users u v", 23);
  CheckErrorAt("SELECT 1 FROM users LEFT JOIN posts p ON 1 = 1", 21);
  CheckErrorAt("SELECT 1 FROM users INNER JOIN posts p ON 1 = 1", 21);
}

TEST(test_parser_reserved_words_cannot_be_identifiers) {
  CheckErrorAt("SELECT select FROM t", 8);
  CheckErrorAt("SELECT id FROM count", 16);
  CheckErrorAt("SELECT count FROM t", 14);
  CheckErrorAt("SELECT id AS where FROM t", 14);
  CheckErrorAt("UPDATE t SET order = 1", 14);
  CheckErrorAt("SELECT u.sum FROM t u", 10);
}

TEST(test_parser_statement_end_accepts_only_blanks_after_semicolon) {
  domain::DeleteStatement trailing_blanks =
      ParseAs<domain::DeleteStatement>("DELETE FROM t;  \n\t");
  domain::DeleteStatement leading_blanks =
      ParseAs<domain::DeleteStatement>("  \tDELETE FROM t");

  CHECK_EQ(trailing_blanks.table, "t");
  CHECK_EQ(leading_blanks.table, "t");
  CheckErrorAt("DELETE FROM t;;", 15);
  CheckErrorAt("DELETE FROM t; #", 16);
  CheckErrorAt("", 1);
  CheckErrorAt("   ", 1);
}

TEST(test_parser_select_list_and_clause_shapes_reject_malformed_input) {
  CheckErrorAt("SELECT * AS everything FROM t", 10);
  CheckErrorAt("SELECT u.* FROM t u", 10);
  CheckErrorAt("SELECT id uid FROM t", 11);
  CheckErrorAt("SELECT id FROM t JOIN u", 24);
  CheckErrorAt("SELECT id FROM t ORDER id", 24);
  CheckErrorAt("SELECT id FROM t GROUP BY", 26);
  CheckErrorAt("SELECT id FROM t ORDER BY id WHERE id = 1", 30);
  CheckErrorAt("SELECT COUNT() FROM t", 14);
  CheckErrorAt("SELECT SUM(*) FROM t", 12);
  CheckErrorAt("SELECT (1 FROM t", 11);
  CheckErrorAt("SELECT 3. FROM t", 9);
  CheckErrorAt("SELECT .5 FROM t", 8);
  CheckErrorAt("SELECT 1 !", 10);
}

TEST(test_parser_malformed_dml_returns_parse_error) {
  CheckErrorAt("INSERT users VALUES (1)", 8);
  CheckErrorAt("INSERT INTO t () VALUES (1)", 16);
  CheckErrorAt("INSERT INTO t VALUES ()", 23);
  CheckErrorAt("INSERT INTO t VALUES (1) (2)", 26);
  CheckErrorAt("INSERT INTO t SELECT 1", 15);
  CheckErrorAt("UPDATE t SET", 13);
  CheckErrorAt("UPDATE t u SET a = 1", 10);
  CheckErrorAt("UPDATE t SET t.a = 1", 15);
  CheckErrorAt("DELETE t", 8);
  CheckErrorAt("DELETE FROM t WHERE", 20);
}

TEST(test_parser_deeply_nested_input_returns_parse_error_without_crashing) {
  constexpr std::size_t kHugeDepth = 100000;
  const std::string many_parentheses = "SELECT " +
                                       std::string(kHugeDepth, '(') + "1" +
                                       std::string(kHugeDepth, ')') + " FROM t";
  const std::string many_minus_signs =
      "SELECT " + std::string(kHugeDepth, '-') + "x FROM t";

  CHECK(!Parses(many_parentheses));
  CHECK(!Parses(many_minus_signs));
  CHECK(!Parses(SubtractionChain(4 * kParserMaxExpressionDepth)));
  CHECK(!Parses(SubtractionChain(kParserMaxExpressionDepth)));
  CHECK(Parses(SubtractionChain(kParserMaxExpressionDepth - 1)));
}

TEST(test_parser_not_supported_words_name_the_word) {
  CHECK_EQ(ParseFailure("SELECT id FROM t WHERE NOT a = 1").description,
           "NOT is not supported");
  CHECK_EQ(ParseFailure("SELECT id FROM t WHERE a NOT LIKE 'x'").description,
           "NOT is not supported");
  CHECK_EQ(ParseFailure("SELECT id FROM t WHERE a IN (1, 2)").description,
           "IN is not supported");
  CHECK_EQ(ParseFailure("SELECT id FROM t WHERE a IN (SELECT 1)").description,
           "IN is not supported");
  CHECK_EQ(ParseFailure("SELECT id FROM t WHERE (SELECT 1) = 1").description,
           "subqueries are not supported");
}

TEST(test_parser_star_item_may_share_the_list) {
  domain::SelectStatement select =
      ParseAs<domain::SelectStatement>("SELECT *, COUNT(*), id, * FROM users");

  CHECK_EQ(select.items.size(), 4u);
  const domain::SelectItem* leading_star = ItemAt(select, 0);
  const domain::SelectItem* count_item = ItemAt(select, 1);
  const domain::SelectItem* column_item = ItemAt(select, 2);
  const domain::SelectItem* trailing_star = ItemAt(select, 3);
  if (leading_star == nullptr || count_item == nullptr ||
      column_item == nullptr || trailing_star == nullptr) {
    return;
  }
  CHECK(leading_star->is_star && leading_star->expr == nullptr);
  CHECK(!count_item->is_star);
  CHECK_EQ(DescribeExpr(count_item->expr.get()), "COUNT(*)");
  CHECK(!column_item->is_star);
  CHECK(trailing_star->is_star);
}

TEST(test_parser_parenthesis_nesting_boundary) {
  // The parser rejects a nesting of 100 levels or more (99 still parses).
  constexpr std::size_t kLastAcceptedDepth = 99;
  const std::string accepted = "SELECT " +
                               std::string(kLastAcceptedDepth, '(') + "1" +
                               std::string(kLastAcceptedDepth, ')') + " FROM t";
  const std::string rejected =
      "SELECT " + std::string(kLastAcceptedDepth + 1, '(') + "1" +
      std::string(kLastAcceptedDepth + 1, ')') + " FROM t";

  CHECK(Parses(accepted));
  CHECK(!Parses(rejected));
}

TEST(test_parser_double_literal_out_of_range_returns_parse_error) {
  const std::string huge_literal = "1" + std::string(400, '0') + ".0";

  CHECK(!Parses("SELECT " + huge_literal + " FROM t"));
  CHECK(Parses("SELECT 1" + std::string(10, '0') + ".0 FROM t"));
}

// ---- DDL ---------------------------------------------------------------

// The only column of `CREATE TABLE t (<definition>)`, or an empty definition
// after reporting a failure.
domain::ColumnDefinition ParseColumnDefinition(const std::string& definition) {
  domain::CreateTableStatement create = ParseAs<domain::CreateTableStatement>(
      "CREATE TABLE t (" + definition + ")");
  if (create.columns.size() != 1) {
    ReportError("expected one column for [" + definition + "]");
    return domain::ColumnDefinition{};
  }
  return std::move(create.columns[0]);
}

TEST(test_parser_create_table_builds_column_definitions) {
  domain::CreateTableStatement create = ParseAs<domain::CreateTableStatement>(
      "CREATE TABLE users (id INT PRIMARY KEY, username TEXT UNIQUE NOT NULL, "
      "location TEXT DEFAULT 'none', score DOUBLE REFERENCES scores(value))");

  CHECK_EQ(create.table, std::string("users"));
  CHECK_EQ(create.columns.size(), std::size_t{4});
  if (create.columns.size() != 4) {
    return;
  }
  const domain::ColumnDefinition& id = create.columns[0];
  const domain::ColumnDefinition& username = create.columns[1];
  const domain::ColumnDefinition& location = create.columns[2];
  const domain::ColumnDefinition& score = create.columns[3];
  CHECK(id.type == domain::DataType::kInt);
  CHECK(id.is_primary_key);
  CHECK(!id.is_unique);
  CHECK(username.type == domain::DataType::kText);
  CHECK(username.is_unique);
  CHECK(!username.is_primary_key);
  CHECK_EQ(DescribeExpr(location.default_value.get()), std::string("'none'"));
  CHECK(score.type == domain::DataType::kDouble);
  CHECK_EQ(score.ref_table, std::string("scores"));
  CHECK_EQ(score.ref_column, std::string("value"));
}

TEST(test_parser_column_types_accept_aliases) {
  domain::CreateTableStatement create = ParseAs<domain::CreateTableStatement>(
      "CREATE TABLE t (a INT, b INTEGER, c BIGINT, d DOUBLE, e REAL, f FLOAT, "
      "g TEXT, h VARCHAR(20), i CHAR(3), j int, k Text)");
  const domain::DataType expected[] = {
      domain::DataType::kInt,    domain::DataType::kInt,
      domain::DataType::kInt,    domain::DataType::kDouble,
      domain::DataType::kDouble, domain::DataType::kDouble,
      domain::DataType::kText,   domain::DataType::kText,
      domain::DataType::kText,   domain::DataType::kInt,
      domain::DataType::kText};
  const char* const bad_types[] = {"VARCHAR", "VARCHAR(0)", "VARCHAR(-1)",
                                   "VARCHAR(x)", "CHAR(1.5)"};

  CHECK_EQ(create.columns.size(), std::size_t{11});
  if (create.columns.size() == 11) {
    for (std::size_t i = 0; i < 11; ++i) {
      CHECK(create.columns[i].type == expected[i]);
    }
  }
  for (const char* type : bad_types) {
    const adapter::ParseError error =
        ParseFailure(std::string("CREATE TABLE t (a ") + type + ")");
    CHECK(error.column >= 1);
  }
}

TEST(test_parser_alter_table_add_column_forms) {
  domain::AddColumnStatement with_keyword = ParseAs<domain::AddColumnStatement>(
      "ALTER TABLE t ADD COLUMN c INT DEFAULT 0");
  domain::AddColumnStatement without_keyword =
      ParseAs<domain::AddColumnStatement>(
          "ALTER TABLE t ADD c TEXT DEFAULT 'x';");

  CHECK_EQ(with_keyword.table, std::string("t"));
  CHECK_EQ(with_keyword.column.name, std::string("c"));
  CHECK(with_keyword.column.type == domain::DataType::kInt);
  CHECK(with_keyword.column.default_value != nullptr);
  CHECK_EQ(without_keyword.table, std::string("t"));
  CHECK_EQ(without_keyword.column.name, std::string("c"));
  CHECK(without_keyword.column.type == domain::DataType::kText);
  CHECK(without_keyword.column.default_value != nullptr);
}

TEST(test_parser_drop_table_builds_statement) {
  domain::DropTableStatement lower =
      ParseAs<domain::DropTableStatement>("DROP TABLE t");
  domain::DropTableStatement upper =
      ParseAs<domain::DropTableStatement>("drop table T;");

  CHECK_EQ(lower.table, std::string("t"));
  CHECK_EQ(upper.table, std::string("T"));
}

TEST(test_parser_invalid_ddl_returns_parse_error) {
  const char* const statements[] = {
      "CREATE TABLE t (a INT PRIMARY KEY PRIMARY KEY)",
      "CREATE TABLE t (a INT UNIQUE UNIQUE)",
      "CREATE TABLE t (a INT AUTOINCREMENT)",
      "CREATE TABLE IF NOT EXISTS t (a INT)",
      "CREATE TABLE t (a INT, PRIMARY KEY (a))",
      "CREATE TABLE t (a INT REFERENCES u)",
      "CREATE TABLE t ()",
      "CREATE TABLE t (a)",
      "CREATE TABLE t (a INT",
      "CREATE TABLE t",
      "ALTER TABLE t DROP COLUMN a",
      "ALTER TABLE t ADD",
      "DROP TABLE",
      "DROP TABLE IF EXISTS t",
  };
  const char* const null_statements[] = {
      "CREATE TABLE t (a INT NULL)",
      "CREATE TABLE t (a INT DEFAULT NULL)",
  };

  for (const char* sql : statements) {
    const adapter::ParseError error = ParseFailure(sql);
    CHECK(error.column >= 1);
    CHECK(!error.description.empty());
  }
  for (const char* sql : null_statements) {
    CHECK_EQ(ParseFailure(sql).description,
             std::string("NULL is not supported"));
  }
}

TEST(test_parser_ddl_words_remain_usable_as_identifiers) {
  domain::SelectStatement select = ParseAs<domain::SelectStatement>(
      "SELECT key, text FROM add WHERE default = 1");
  domain::CreateTableStatement create = ParseAs<domain::CreateTableStatement>(
      "CREATE TABLE table (key INT PRIMARY KEY, text TEXT, default INT "
      "DEFAULT 1)");

  CHECK_EQ(select.from.table, std::string("add"));
  CHECK_EQ(select.items.size(), std::size_t{2});
  CHECK_EQ(create.table, std::string("table"));
  CHECK_EQ(create.columns.size(), std::size_t{3});
  if (create.columns.size() != 3) {
    return;
  }
  CHECK_EQ(create.columns[0].name, std::string("key"));
  CHECK_EQ(create.columns[1].name, std::string("text"));
  CHECK_EQ(create.columns[2].name, std::string("default"));
}

TEST(test_parser_default_takes_one_expression) {
  domain::ColumnDefinition negative =
      ParseColumnDefinition("a INT DEFAULT -1 NOT NULL");
  domain::ColumnDefinition sum = ParseColumnDefinition("a INT DEFAULT 1 + 2");
  domain::ColumnDefinition text =
      ParseColumnDefinition("a TEXT DEFAULT 'x' UNIQUE");
  domain::ColumnDefinition reference =
      ParseColumnDefinition("a INT REFERENCES u(id) DEFAULT 0");

  CHECK_EQ(DescribeExpr(negative.default_value.get()), std::string("-1"));
  CHECK_EQ(DescribeExpr(sum.default_value.get()), std::string("ADD(1, 2)"));
  CHECK_EQ(DescribeExpr(text.default_value.get()), std::string("'x'"));
  CHECK(text.is_unique);
  CHECK_EQ(reference.ref_table, std::string("u"));
  CHECK_EQ(DescribeExpr(reference.default_value.get()), std::string("0"));
}

// ---- Stage 1d: operators, functions, LIMIT, multi-row VALUES
// -------------------

namespace {

// 1-based column of the first occurrence of needle in sql.
std::size_t ColumnOf(const std::string& sql, const std::string& needle) {
  const std::size_t offset = sql.find(needle);
  if (offset == std::string::npos) {
    ReportError("[" + sql + "] does not contain [" + needle + "]");
    return 0;
  }
  return offset + 1;
}

// `SELECT LOWER(LOWER(... a ...)) FROM t` with the given number of calls.
std::string NestedCalls(std::size_t calls) {
  std::string text = "SELECT ";
  for (std::size_t i = 0; i < calls; ++i) {
    text += "LOWER(";
  }
  text += "a";
  text += std::string(calls, ')');
  return text + " FROM t";
}

// `SELECT a || a || ... FROM t` with the given number of operators.
std::string ConcatChain(std::size_t operators) {
  std::string text = "SELECT a";
  for (std::size_t i = 0; i < operators; ++i) {
    text += " || a";
  }
  return text + " FROM t";
}

// The error has exactly the expected column and description.
void CheckError(const std::string& sql, std::size_t expected_column,
                const std::string& expected_description) {
  const adapter::ParseError error = ParseFailure(sql);
  if (error.column != expected_column ||
      error.description != expected_description) {
    ReportError("[" + sql + "] failed at column " +
                std::to_string(error.column) + " with [" + error.description +
                "], expected column " + std::to_string(expected_column) +
                " with [" + expected_description + "]");
  }
}

}  // namespace

TEST(test_parser_multiplicative_and_concat_precedence) {
  const ShapeCase cases[] = {
      {"1 + 2 * 3", "ADD(1, MUL(2, 3))"},
      {"a * b + c", "ADD(MUL(a, b), c)"},
      {"8 / 2 / 2", "DIV(DIV(8, 2), 2)"},
      {"a * b || c", "MUL(a, CONCAT(b, c))"},
      {"a || b || c", "CONCAT(CONCAT(a, b), c)"},
      {"-a * 2", "MUL(SUB(0, a), 2)"},
      {"2 * -3", "MUL(2, -3)"},
      {"a - b / c", "SUB(a, DIV(b, c))"},
  };

  for (const ShapeCase& shape_case : cases) {
    CheckShape(shape_case.text, shape_case.shape);
  }
}

TEST(test_parser_select_star_and_multiplication_are_distinguished) {
  domain::SelectStatement only_star =
      ParseAs<domain::SelectStatement>("SELECT * FROM t");
  domain::SelectStatement product =
      ParseAs<domain::SelectStatement>("SELECT a * b FROM t");
  domain::SelectStatement trailing_star =
      ParseAs<domain::SelectStatement>("SELECT a, * FROM t");
  domain::SelectStatement leading_star =
      ParseAs<domain::SelectStatement>("SELECT *, a * 2 FROM t");

  CHECK_EQ(only_star.items.size(), std::size_t{1});
  CHECK(only_star.items.size() == 1 && only_star.items[0].is_star);
  CHECK_EQ(product.items.size(), std::size_t{1});
  CHECK(product.items.size() == 1 && !product.items[0].is_star);
  if (product.items.size() == 1) {
    CHECK_EQ(DescribeExpr(product.items[0].expr.get()), "MUL(a, b)");
  }
  CHECK_EQ(trailing_star.items.size(), std::size_t{2});
  CHECK(trailing_star.items.size() == 2 && !trailing_star.items[0].is_star &&
        trailing_star.items[1].is_star);
  CHECK_EQ(leading_star.items.size(), std::size_t{2});
  CHECK(leading_star.items.size() == 2 && leading_star.items[0].is_star &&
        !leading_star.items[1].is_star);
  if (leading_star.items.size() == 2) {
    CHECK_EQ(DescribeExpr(leading_star.items[1].expr.get()), "MUL(a, 2)");
  }
  CheckShape("COUNT(*) * 2", "MUL(COUNT(*), 2)");
}

TEST(test_parser_like_builds_binary_node_and_does_not_chain) {
  CheckShape("a LIKE 'x%'", "LIKE(a, 'x%')");
  CheckShape("a LIKE b || 'x'", "LIKE(a, CONCAT(b, 'x'))");
  CheckShape("a = 1 AND b LIKE 'y'", "AND(EQ(a, 1), LIKE(b, 'y'))");

  const char* const chained[] = {
      "SELECT id FROM t WHERE a LIKE b LIKE c",
      "SELECT id FROM t WHERE a LIKE b = c",
      "SELECT id FROM t WHERE a = b LIKE c",
  };
  for (const char* sql : chained) {
    const adapter::ParseError error = ParseFailure(sql);
    CHECK(error.description.find("cannot be chained") != std::string::npos);
  }
}

TEST(test_parser_function_calls_build_function_nodes) {
  const ShapeCase cases[] = {
      {"LOWER(a)", "LOWER(a)"},
      {"upper(a)", "UPPER(a)"},
      {"LENGTH(a)", "LENGTH(a)"},
      {"SUBSTR(a, 1)", "SUBSTR(a, 1)"},
      {"SUBSTR(a, 1, 2)", "SUBSTR(a, 1, 2)"},
      {"TRIM(a)", "TRIM(a)"},
      {"ABS(a)", "ABS(a)"},
      {"ROUND(a)", "ROUND(a)"},
      {"ROUND(a, 2)", "ROUND(a, 2)"},
      {"CAST(a AS INT)", "CAST(a AS INT)"},
      {"CAST(a AS double)", "CAST(a AS DOUBLE)"},
      {"CAST(a AS VARCHAR(10))", "CAST(a AS TEXT)"},
      {"UPPER(TRIM(a))", "UPPER(TRIM(a))"},
      {"LOWER(a, b)", "LOWER(a, b)"},
  };

  for (const ShapeCase& shape_case : cases) {
    CheckShape(shape_case.text, shape_case.shape);
  }
  domain::ExprPtr substr = ParseExpressionText("SUBSTR(a, 1, 2)");
  const auto* call = substr == nullptr
                         ? nullptr
                         : std::get_if<domain::FunctionCall>(&substr->node);
  CHECK(call != nullptr);
  if (call != nullptr) {
    CHECK(call->fn == domain::ScalarFn::kSubstr);
    CHECK_EQ(call->args.size(), std::size_t{3});
  }
  domain::ExprPtr cast = ParseExpressionText("CAST(a AS double)");
  const auto* cast_call = cast == nullptr
                              ? nullptr
                              : std::get_if<domain::FunctionCall>(&cast->node);
  CHECK(cast_call != nullptr);
  if (cast_call != nullptr) {
    CHECK(cast_call->fn == domain::ScalarFn::kCast);
    CHECK(cast_call->cast_type == domain::DataType::kDouble);
  }
}

TEST(test_parser_aggregates_avg_min_max_and_plain_columns) {
  CheckShape("AVG(a)", "AVG(a)");
  CheckShape("min(a)", "MIN(a)");
  CheckShape("MAX(a + 1)", "MAX(ADD(a, 1))");
  CheckShape("COUNT(*)", "COUNT(*)");
  domain::ExprPtr avg = ParseExpressionText("AVG(a)");
  CHECK(AsAggregate(avg.get()) != nullptr &&
        AsAggregate(avg.get())->fn == domain::AggregateFn::kAvg);
  domain::ExprPtr count_star = ParseExpressionText("COUNT(*)");
  CHECK(AsAggregate(count_star.get()) != nullptr &&
        AsAggregate(count_star.get())->arg == nullptr);

  CheckShape("min", "min");
  domain::SelectStatement columns =
      ParseAs<domain::SelectStatement>("SELECT t.max, length FROM t");
  CHECK_EQ(columns.items.size(), std::size_t{2});
  if (columns.items.size() == 2) {
    CHECK_EQ(DescribeExpr(columns.items[0].expr.get()), "t.max");
    CHECK_EQ(DescribeExpr(columns.items[1].expr.get()), "length");
  }
  CheckError("SELECT AVG(*) FROM t", ColumnOf("SELECT AVG(*) FROM t", "*"),
             "expected an expression, found '*'");
}

TEST(test_parser_function_errors_report_position_and_description) {
  CheckError("SELECT FOO(1) FROM t", 8, "unknown function FOO");
  const std::string spaced = "SELECT id FROM t WHERE x = foo (1)";
  CheckError(spaced, ColumnOf(spaced, "foo"), "unknown function foo");
  const std::string missing_as = "SELECT CAST(a) FROM t";
  CheckError(missing_as, ColumnOf(missing_as, ")"), "expected 'AS', found ')'");
  const std::string bad_type = "SELECT CAST(a AS BOOL) FROM t";
  CheckError(bad_type, ColumnOf(bad_type, "BOOL"),
             "expected a column type, found 'BOOL'");
  const std::string unclosed = "SELECT CAST(a AS INT";
  CheckError(unclosed, unclosed.size() + 1, "expected ')', found end of input");
  const std::string no_arguments = "SELECT LOWER() FROM t";
  CheckError(no_arguments, ColumnOf(no_arguments, ")"),
             "expected an expression, found ')'");
  const std::string not_call = "SELECT id FROM t WHERE NOT (a = 1)";
  CheckError(not_call, ColumnOf(not_call, "NOT"), "NOT is not supported");
}

TEST(test_parser_limit_and_offset_build_select_fields) {
  struct LimitCase {
    const char* sql;
    bool has_limit;
    int64_t limit;
    int64_t offset;
  };
  const LimitCase cases[] = {
      {"SELECT id FROM t LIMIT 5", true, 5, 0},
      {"SELECT id FROM t LIMIT 5 OFFSET 2", true, 5, 2},
      {"SELECT id FROM t LIMIT 0", true, 0, 0},
      {"SELECT id FROM t ORDER BY id LIMIT 3", true, 3, 0},
      {"SELECT id FROM t LIMIT 9223372036854775807", true, kInt64Max, 0},
      {"SELECT id FROM t WHERE a = 1 LIMIT 1;", true, 1, 0},
      {"SELECT id FROM t", false, 0, 0},
  };

  for (const LimitCase& limit_case : cases) {
    domain::SelectStatement select =
        ParseAs<domain::SelectStatement>(limit_case.sql);
    CHECK_EQ(select.limit.has_value(), limit_case.has_limit);
    if (select.limit.has_value() && limit_case.has_limit) {
      CHECK_EQ(*select.limit, limit_case.limit);
    }
    CHECK_EQ(select.offset, limit_case.offset);
  }
}

TEST(test_parser_limit_and_offset_reject_invalid_forms) {
  const std::string limit_text =
      "LIMIT requires a non-negative integer literal";
  const std::string offset_text =
      "OFFSET requires a non-negative integer literal";
  const std::string base = "SELECT id FROM t ";

  CheckError(base + "LIMIT -1", ColumnOf(base + "LIMIT -1", "-"), limit_text);
  CheckError(base + "LIMIT 1.5", ColumnOf(base + "LIMIT 1.5", "1.5"),
             limit_text);
  CheckError(base + "LIMIT x", ColumnOf(base + "LIMIT x", "x"), limit_text);
  CheckError(base + "LIMIT", (base + "LIMIT").size() + 1, limit_text);
  CheckError(base + "LIMIT 1 + 1", ColumnOf(base + "LIMIT 1 + 1", "+"),
             "expected ';' or end of input, found '+'");
  CheckError(base + "OFFSET 2", ColumnOf(base + "OFFSET 2", "OFFSET"),
             "OFFSET requires LIMIT");
  CheckError(base + "LIMIT 5 OFFSET -1",
             ColumnOf(base + "LIMIT 5 OFFSET -1", "-"), offset_text);
  CheckError(base + "LIMIT 9223372036854775808",
             ColumnOf(base + "LIMIT 9223372036854775808", "9223"),
             "numeric literal out of range");
  CheckError(base + "LIMIT 5 LIMIT 6",
             ColumnOf(base + "LIMIT 5 LIMIT 6", "LIMIT 6"),
             "expected ';' or end of input, found 'LIMIT'");
}

TEST(test_parser_multi_row_values_build_rows) {
  domain::InsertStatement plain = ParseAs<domain::InsertStatement>(
      "INSERT INTO t VALUES (1, 'a'), (2, 'b'), (3, 'c')");
  domain::InsertStatement listed = ParseAs<domain::InsertStatement>(
      "INSERT INTO t (a, b) VALUES (1, 'a'), (2, 'b'), (3, 'c')");
  domain::InsertStatement uneven =
      ParseAs<domain::InsertStatement>("INSERT INTO t VALUES (1), (2, 3)");

  CHECK_EQ(plain.rows.size(), std::size_t{3});
  CHECK_EQ(listed.rows.size(), std::size_t{3});
  CHECK_EQ(listed.columns.size(), std::size_t{2});
  for (const auto& row : plain.rows) {
    CHECK_EQ(row.size(), std::size_t{2});
  }
  for (const auto& row : listed.rows) {
    CHECK_EQ(row.size(), std::size_t{2});
  }
  CHECK_EQ(uneven.rows.size(), std::size_t{2});
  if (uneven.rows.size() == 2) {
    CHECK_EQ(uneven.rows[0].size(), std::size_t{1});
    CHECK_EQ(uneven.rows[1].size(), std::size_t{2});
  }
  CHECK(!Parses("INSERT INTO t VALUES (1),"));
  CHECK(!Parses("INSERT INTO t VALUES (1) (2)"));
  CHECK(!Parses("INSERT INTO t VALUES"));
}

TEST(test_parser_concat_and_division_operators_lex) {
  CheckShape("a || b", "CONCAT(a, b)");
  CheckShape("a||b", "CONCAT(a, b)");
  CheckShape("a / b", "DIV(a, b)");
  CheckShape("a/b", "DIV(a, b)");
  const std::string single_bar = "SELECT a | b FROM t";
  CheckError(single_bar, ColumnOf(single_bar, "|"), "unexpected character '|'");
  const std::string triple_bar = "SELECT a ||| b FROM t";
  CheckError(triple_bar, ColumnOf(triple_bar, "|||") + 2,
             "unexpected character '|'");
}

TEST(test_parser_function_and_concat_depth_limits) {
  // A select item is level 1, so 99 nested calls parse and 100 do not.
  constexpr std::size_t kLastAcceptedCalls = 99;

  CHECK(Parses(NestedCalls(kLastAcceptedCalls)));
  CheckError(NestedCalls(kLastAcceptedCalls + 1),
             NestedCalls(kLastAcceptedCalls + 1).find('a') + 1,
             "expression is nested too deeply");
  // A chain of n operators has depth n + 1 and the limit is 500.
  CHECK(Parses(ConcatChain(kParserMaxExpressionDepth - 1)));
  const std::string too_deep = ConcatChain(kParserMaxExpressionDepth);
  CHECK(!Parses(too_deep));
  if (!Parses(too_deep)) {
    CHECK_EQ(ParseFailure(too_deep).description,
             std::string("expression is nested too deeply"));
  }
}

// ---- Stage 1e: comments, non-ASCII spaces, unexpected characters ------------

TEST(test_parser_comments_run_to_end_of_line) {
  domain::SelectStatement commented =
      ParseAs<domain::SelectStatement>("SELECT a -- x\nFROM t");
  CHECK_EQ(commented.from.table, std::string("t"));
  CHECK_EQ(commented.items.size(), std::size_t{1});

  domain::ExprPtr literal = ParseExpressionText("'--'");
  const domain::Value* value = LiteralValue(literal.get());
  CHECK(value != nullptr && std::holds_alternative<std::string>(*value));
  if (value != nullptr) {
    CHECK_EQ(DescribeValue(*value), "'--'");
  }

  CheckShape("1 - -2", "SUB(1, -2)");
  // `--2 FROM t` is a comment, so the statement ends after `SELECT 1`.
  const std::string commented_out = "SELECT 1--2 FROM t";
  CheckError(commented_out, commented_out.size() + 1,
             "expected FROM, found end of input");
  CheckError("-- only", 1, "unrecognized statement");
}

TEST(test_parser_treats_non_ascii_spaces_as_whitespace) {
  const std::string ideographic_space = "\xE3\x80\x80";
  const std::string no_break_space = "\xC2\xA0";
  const std::string em_space = "\xE2\x80\x83";

  domain::SelectStatement spaced = ParseAs<domain::SelectStatement>(
      "SELECT" + ideographic_space + "a" + no_break_space + "FROM" + em_space +
      "t");
  domain::SelectStatement trailing =
      ParseAs<domain::SelectStatement>("SELECT a FROM t" + ideographic_space);

  CHECK_EQ(spaced.from.table, std::string("t"));
  CHECK_EQ(trailing.from.table, std::string("t"));

  // The ends of U+2000 to U+200A are blank; the code points next to them are
  // not.
  const std::string en_quad = "\xE2\x80\x80";
  const std::string hair_space = "\xE2\x80\x8A";
  domain::SelectStatement range_ends = ParseAs<domain::SelectStatement>(
      "SELECT" + en_quad + "a FROM" + hair_space + "t");
  CHECK_EQ(range_ends.from.table, std::string("t"));
  CheckError("SELECT a FROM t\xE2\x80\x8B", 16, "unexpected character U+200B");
  CheckError("SELECT a FROM t\xE1\xBF\xBF", 16, "unexpected character U+1FFF");
  CheckError("SELECT a FROM t\xE3\x80\x81", 16, "unexpected character U+3001");
  CheckError("SELECT a FROM t\xC2\xA1", 16, "unexpected character U+00A1");
}

TEST(test_parser_unexpected_character_reports_code_point) {
  const std::string right_quote = "\xE2\x80\x99";
  const std::string quoted =
      "SELECT " + right_quote + "a" + right_quote + " FROM t";

  CheckError(quoted, 8, "unexpected character U+2019");
  CheckError("SELECT # FROM t", 8, "unexpected character '#'");
  CheckError("SELECT \xFF FROM t", 8,
             "unexpected character with byte value 255");
  CheckError("SELECT \x01 FROM t", 8, "unexpected character with byte value 1");
}
