#ifndef FLATFILEDB_DOMAIN_AST_H_
#define FLATFILEDB_DOMAIN_AST_H_

#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "flatfiledb/domain/value.h"

namespace flatfiledb::domain {

enum class BinaryOp { kEq, kNe, kLt, kLe, kGt, kGe, kAnd, kOr, kAdd, kSub };
enum class AggregateFn { kCount, kSum };

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

// A struct, not a variant alias: the alternatives refer back to Expr.
struct Expr {
  std::variant<Literal, ColumnRef, BinaryExpr, AggregateCall> node;
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
};

struct InsertStatement {
  std::string table;
  std::vector<std::string> columns;  // empty: all columns in schema order
  std::vector<ExprPtr> values;
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

}  // namespace flatfiledb::domain

#endif  // FLATFILEDB_DOMAIN_AST_H_
