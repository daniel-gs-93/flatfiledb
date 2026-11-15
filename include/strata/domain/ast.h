#ifndef STRATA_DOMAIN_AST_H_
#define STRATA_DOMAIN_AST_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "strata/domain/value.h"

namespace strata::domain {

enum class BinaryOp {
  kEq,
  kNe,
  kLt,
  kLe,
  kGt,
  kGe,
  kAnd,
  kOr,
  kAdd,
  kSub,
  kMul,
  kDiv,
  kConcat,
  kLike
};
enum class AggregateFn { kCount, kSum, kAvg, kMin, kMax };
// The scalar functions of docs/DesignDocument.md section 2.3. kCast is
// CAST(x AS type): its target type is FunctionCall::cast_type.
enum class ScalarFn {
  kLower,
  kUpper,
  kLength,
  kSubstr,
  kTrim,
  kAbs,
  kRound,
  kCast
};

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct Literal {
  Value value;
};

struct ColumnRef {
  std::string qualifier;  // empty when the column is not qualified
  std::string name;
};

struct BinaryExpr {
  BinaryOp op;
  ExprPtr lhs;
  ExprPtr rhs;
};

struct AggregateCall {
  AggregateFn fn;
  ExprPtr arg;  // null for COUNT(*)
};

struct FunctionCall {
  ScalarFn fn;
  std::vector<ExprPtr> args;             // the parser does not check the count
  DataType cast_type = DataType::kText;  // only read when fn is kCast
};

// A struct, not a variant alias: the alternatives refer back to Expr.
struct Expr {
  std::variant<Literal, ColumnRef, BinaryExpr, AggregateCall, FunctionCall>
      node;
};

struct SelectItem {
  bool is_star;
  ExprPtr expr;       // null when is_star
  std::string alias;  // empty when absent
  std::string text;   // source text of the expression, used as column name
};

struct TableRef {
  std::string table;
  std::string alias;  // empty when absent
};

struct JoinClause {
  TableRef table;
  ExprPtr on;
};

struct OrderItem {
  ExprPtr expr;
  bool is_descending;
};

struct Assignment {
  std::string column;
  ExprPtr value;
};

struct SelectStatement {
  std::vector<SelectItem> items;
  TableRef from;
  std::vector<JoinClause> joins;
  ExprPtr where;  // null when absent
  std::vector<ExprPtr> group_by;
  std::vector<OrderItem> order_by;
  std::optional<int64_t> limit = std::nullopt;  // absent: all rows
  int64_t offset = 0;
};

struct InsertStatement {
  std::string table;
  std::vector<std::string> columns;        // empty: all columns in schema order
  std::vector<std::vector<ExprPtr>> rows;  // one expression list per VALUES row
};

struct UpdateStatement {
  std::string table;
  std::vector<Assignment> assignments;
  ExprPtr where;  // null: every row
};

struct DeleteStatement {
  std::string table;
  ExprPtr where;  // null: every row
};

// One column of CREATE TABLE or ALTER TABLE ADD COLUMN.
struct ColumnDefinition {
  std::string name;
  DataType type;
  bool is_primary_key = false;
  bool is_unique = false;
  bool is_auto_increment = false;
  ExprPtr default_value;  // null when absent
  std::string ref_table;  // empty when the column has no REFERENCES
  std::string ref_column;
};

struct CreateTableStatement {
  std::string table;
  std::vector<ColumnDefinition> columns;
};

struct AddColumnStatement {
  std::string table;
  ColumnDefinition column;
};

struct DropTableStatement {
  std::string table;
};

using Statement =
    std::variant<SelectStatement, InsertStatement, UpdateStatement,
                 DeleteStatement, CreateTableStatement, AddColumnStatement,
                 DropTableStatement>;

}  // namespace strata::domain

#endif  // STRATA_DOMAIN_AST_H_
