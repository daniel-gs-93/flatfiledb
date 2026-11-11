#ifndef FLATFILEDB_DOMAIN_PLAN_H_
#define FLATFILEDB_DOMAIN_PLAN_H_

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "flatfiledb/domain/ast.h"
#include "flatfiledb/domain/value.h"

namespace flatfiledb::domain {

// Static type of a bound expression. kBool exists only here: a Value is never
// a boolean.
enum class ExprType { kBool, kInt, kDouble, kText };

// The storable type for a non-boolean static type. Precondition: type is not
// kBool.
inline DataType DataTypeOf(ExprType type) {
  switch (type) {
    case ExprType::kInt:
      return DataType::kInt;
    case ExprType::kDouble:
      return DataType::kDouble;
    default:
      return DataType::kText;
  }
}

struct BoundExpr;
using BoundExprPtr = std::unique_ptr<BoundExpr>;

struct BoundLiteral {
  Value value;
};

// Position in the row the expression is evaluated against. Above a JoinNode
// the row is the left row followed by the right row.
struct BoundColumn {
  std::size_t index;
};

struct BoundBinary {
  BinaryOp op;
  BoundExprPtr lhs;
  BoundExprPtr rhs;
};

// A scalar function call. The result type is BoundExpr::type, which for kCast
// is the target type. The Binder has checked the argument count and types.
struct BoundFunction {
  ScalarFn fn;
  std::vector<BoundExprPtr> args;
};

// Only the Binder produces this form, while it rewrites select-list items
// over an AggregateNode. It never appears in a plan returned by Bind().
struct BoundAggregate {
  AggregateFn fn;
  BoundExprPtr arg;  // null for COUNT(*)
};

struct BoundExpr {
  ExprType type;
  std::variant<BoundLiteral, BoundColumn, BoundBinary, BoundFunction,
               BoundAggregate>
      node;
};

struct PlanNode;
using PlanPtr = std::unique_ptr<PlanNode>;

struct ScanNode {
  std::string table;  // spelling declared by the catalog
};

struct FilterNode {
  PlanPtr child;
  BoundExprPtr predicate;
};

struct JoinNode {
  PlanPtr left;
  PlanPtr right;
  BoundExprPtr condition;  // over the left row followed by the right row
};

struct AggregateSpec {
  AggregateFn fn;
  BoundExprPtr arg;  // null for COUNT(*)
};

// Output rows are the group keys followed by one value per aggregate.
struct AggregateNode {
  PlanPtr child;
  std::vector<BoundExprPtr> group_keys;
  std::vector<AggregateSpec> aggregates;
};

struct ProjectNode {
  PlanPtr child;
  std::vector<BoundExprPtr> exprs;
  std::vector<ColumnDef> output_columns;
};

struct SortKey {
  std::size_t column_index;  // position in the ProjectNode output
  bool is_descending;
};

struct SortNode {
  PlanPtr child;
  std::vector<SortKey> keys;
};

// Skips the first offset rows of the child, then keeps at most limit rows.
struct LimitNode {
  PlanPtr child;
  std::optional<int64_t> limit;  // absent: no upper bound
  int64_t offset;
};

// Each row holds one value per schema column, in schema order, already
// converted to the column type. There is at least one row.
// When generated_index is set, row[*generated_index] is a placeholder in every
// row: the engine replaces it with the next value of the auto-increment column,
// in row order.
struct InsertNode {
  std::string table;
  std::vector<Row> rows;
  std::optional<std::size_t> generated_index = std::nullopt;
};

struct BoundAssignment {
  std::size_t column_index;
  BoundExprPtr value;  // evaluated against the row before the update
};

struct UpdateNode {
  std::string table;
  std::vector<BoundAssignment> assignments;
  BoundExprPtr predicate;  // null: every row
};

struct DeleteNode {
  std::string table;
  BoundExprPtr predicate;  // null: every row
};

// DDL. The schema of CreateTableNode is complete: foreign keys ascending by
// column position, unique_indexes without the primary key.
struct CreateTableNode {
  TableSchema schema;
};

// column carries its default value, which ADD COLUMN requires.
struct AddColumnNode {
  std::string table;
  ColumnDef column;
};

struct DropTableNode {
  std::string table;
};

// A struct, not a variant alias: the alternatives refer back to PlanNode.
struct PlanNode {
  std::variant<ScanNode, FilterNode, JoinNode, AggregateNode, ProjectNode,
               SortNode, LimitNode, InsertNode, UpdateNode, DeleteNode,
               CreateTableNode, AddColumnNode, DropTableNode>
      node;
};

}  // namespace flatfiledb::domain

#endif  // FLATFILEDB_DOMAIN_PLAN_H_
