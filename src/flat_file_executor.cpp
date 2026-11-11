#include "flatfiledb/flat_file_executor.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "flatfiledb/domain/expression.h"

// Pure logic: every function here reads a TableStore and returns a value. It
// never touches a file or the repository; FlatFileEngine does that.
//
// Container costs (n = rows scanned, m = rows on the other join side, k =
// pairs that survive the join and its filter):
//   Scan copies the rows, O(n). Hash join: build O(m), probe O(n + matching
//   candidates), result memory O(k). Nested loop: O(n * m) predicate
//   evaluations, result memory O(k). Aggregate: one unordered_map lookup per
//   row, O(n). Sort: std::stable_sort, O(n log n). INSERT checks scan the
//   table, O(n); UPDATE and DELETE build one unordered_set per checked column,
//   O(n + m).

namespace flatfiledb::infrastructure {

namespace {

using domain::BoundBinary;
using domain::BoundColumn;
using domain::BoundExpr;
using domain::BoundExprPtr;
using domain::ColumnDef;
using domain::DataType;
using domain::ErrorKind;
using domain::ExecutionError;
using domain::ExprType;
using domain::ForeignKey;
using domain::PlanNode;
using domain::Row;
using domain::TableSchema;
using domain::Value;

ExecutionError MakeError(ErrorKind kind, std::string message) {
  return ExecutionError{kind, std::move(message)};
}

ExecutionError OverflowError() {
  return MakeError(ErrorKind::kRuntime, "numeric overflow");
}

// The kRuntime error for a failed evaluation; failure is the message that
// domain::Evaluate or domain::EvaluateCondition wrote.
ExecutionError EvaluationError(std::string failure) {
  return MakeError(ErrorKind::kRuntime, std::move(failure));
}

const char* AggregateFnName(domain::AggregateFn fn) {
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

ExecutionError UnknownTableError(const std::string& name) {
  return MakeError(ErrorKind::kBind, "unknown table: " + name);
}

// ----------------------------------------------------------------------------
// Keys for hash tables.
// ----------------------------------------------------------------------------

void AppendBytes(const void* bytes, std::size_t size, std::string* key) {
  key->append(static_cast<const char*>(bytes), size);
}

void AppendDouble(double number, std::string* key) {
  // -0.0 and 0.0 compare equal, so they must produce the same bytes.
  if (number == 0.0) {
    number = 0.0;
  }
  AppendBytes(&number, sizeof(number), key);
}

// Exact encoding: equal strings if and only if the values have the same type
// and the same content. Self-delimiting, so concatenating the encodings of
// several values cannot collide.
void AppendExactKey(const Value& value, std::string* key) {
  if (const auto* integer = std::get_if<int64_t>(&value)) {
    key->push_back('I');
    AppendBytes(integer, sizeof(*integer), key);
    return;
  }
  if (const auto* number = std::get_if<double>(&value)) {
    key->push_back('D');
    AppendDouble(*number, key);
    return;
  }
  // Value has three alternatives and the INT and DOUBLE ones were handled
  // above, so the value holds a string.
  const std::string& text = std::get<std::string>(value);
  std::uint64_t length = text.size();
  key->push_back('T');
  AppendBytes(&length, sizeof(length), key);
  key->append(text);
}

std::string ExactKey(const Value& value) {
  std::string key;
  AppendExactKey(value, &key);
  return key;
}

// Join encoding: INT and DOUBLE share one key space through their double
// value, so 2 and 2.0 land in the same bucket. Two large INTs can share a
// double, so a bucket hit must be confirmed with ValuesEqual.
std::string JoinKey(const Value& value) {
  if (const auto* text = std::get_if<std::string>(&value)) {
    return "T" + *text;
  }
  std::string key = "N";
  AppendDouble(domain::ToDouble(value), &key);
  return key;
}

// ----------------------------------------------------------------------------
// Query execution.
// ----------------------------------------------------------------------------

struct RowSet {
  std::vector<ColumnDef> columns;
  std::vector<Row> rows;
};

using RowSetResult = std::variant<RowSet, ExecutionError>;

RowSetResult ExecuteNode(const PlanNode& plan, const TableStore& store);

RowSetResult ExecuteScan(const domain::ScanNode& scan,
                         const TableStore& store) {
  const TableData* table = store.Find(scan.table);
  if (table == nullptr) {
    return UnknownTableError(scan.table);
  }
  return RowSet{table->schema.columns, table->rows};
}

// Executes plan into *output. Returns the error when execution fails; *output
// is written only on success.
std::optional<ExecutionError> ExecuteChild(const PlanNode& plan,
                                           const TableStore& store,
                                           RowSet* output) {
  RowSetResult result = ExecuteNode(plan, store);
  if (auto* error = std::get_if<ExecutionError>(&result)) {
    return std::move(*error);
  }
  // The variant has two alternatives and the error one was ruled out above.
  *output = std::move(std::get<RowSet>(result));
  return std::nullopt;
}

RowSetResult ExecuteJoin(const domain::JoinNode& join,
                         const BoundExpr* fused_filter,
                         const TableStore& store);

RowSetResult ExecuteFilter(const domain::FilterNode& filter,
                           const TableStore& store) {
  // A join and the filter above it run as one operation, so a pair the filter
  // rejects is never materialized.
  if (const auto* join = std::get_if<domain::JoinNode>(&filter.child->node)) {
    return ExecuteJoin(*join, filter.predicate.get(), store);
  }
  RowSet input;
  if (auto error = ExecuteChild(*filter.child, store, &input)) {
    return std::move(*error);
  }

  RowSet output{std::move(input.columns), {}};
  for (Row& row : input.rows) {
    std::string failure;
    std::optional<bool> is_match =
        domain::EvaluateCondition(*filter.predicate, row, &failure);
    if (!is_match) {
      return EvaluationError(std::move(failure));
    }
    if (*is_match) {
      output.rows.push_back(std::move(row));
    }
  }
  return output;
}

// Positions of the two columns of an equality join condition, the second
// relative to the start of the right row.
struct EquiJoinColumns {
  std::size_t left_index;
  std::size_t right_index;
};

// condition is one conjunct. Returns the columns when it is an equality
// between a left column and a right column, in either operand order.
std::optional<EquiJoinColumns> FindEquiJoinColumns(const BoundExpr& condition,
                                                   std::size_t left_width,
                                                   std::size_t right_width) {
  const auto* binary = std::get_if<BoundBinary>(&condition.node);
  if (binary == nullptr || binary->op != domain::BinaryOp::kEq) {
    return std::nullopt;
  }
  const auto* lhs = std::get_if<BoundColumn>(&binary->lhs->node);
  const auto* rhs = std::get_if<BoundColumn>(&binary->rhs->node);
  if (lhs == nullptr || rhs == nullptr) {
    return std::nullopt;
  }

  std::size_t total_width = left_width + right_width;
  if (lhs->index >= total_width || rhs->index >= total_width) {
    return std::nullopt;
  }
  if (lhs->index < left_width && rhs->index >= left_width) {
    return EquiJoinColumns{lhs->index, rhs->index - left_width};
  }
  if (rhs->index < left_width && lhs->index >= left_width) {
    return EquiJoinColumns{rhs->index, lhs->index - left_width};
  }
  return std::nullopt;
}

// How one join runs: an optional hash key, and the predicates every candidate
// pair must satisfy. The predicates are borrowed from the plan, which outlives
// the join.
struct JoinSpec {
  std::optional<EquiJoinColumns> equi_columns;
  std::vector<const BoundExpr*> residuals;
};

// Appends the operands of a top-level AND chain, left to right. expr is
// borrowed and must outlive conjuncts.
void CollectConjuncts(const BoundExpr& expr,
                      std::vector<const BoundExpr*>* conjuncts) {
  const auto* binary = std::get_if<BoundBinary>(&expr.node);
  if (binary == nullptr || binary->op != domain::BinaryOp::kAnd) {
    conjuncts->push_back(&expr);
    return;
  }
  CollectConjuncts(*binary->lhs, conjuncts);
  CollectConjuncts(*binary->rhs, conjuncts);
}

// The first ON conjunct that equates a left column with a right column is the
// hash key; the other conjuncts and the fused filter are residuals. Both
// pointers may be null.
JoinSpec MakeJoinSpec(const BoundExpr* condition, const BoundExpr* fused_filter,
                      std::size_t left_width, std::size_t right_width) {
  JoinSpec spec;
  std::vector<const BoundExpr*> conjuncts;
  if (condition != nullptr) {
    CollectConjuncts(*condition, &conjuncts);
  }
  for (const BoundExpr* conjunct : conjuncts) {
    if (!spec.equi_columns) {
      spec.equi_columns =
          FindEquiJoinColumns(*conjunct, left_width, right_width);
      if (spec.equi_columns) {
        continue;
      }
    }
    spec.residuals.push_back(conjunct);
  }
  if (fused_filter != nullptr) {
    spec.residuals.push_back(fused_filter);
  }
  return spec;
}

Row Concatenate(const Row& left, const Row& right) {
  Row combined;
  combined.reserve(left.size() + right.size());
  combined.insert(combined.end(), left.begin(), left.end());
  combined.insert(combined.end(), right.begin(), right.end());
  return combined;
}

// Right-row positions per join key, each list ascending.
using JoinBuckets = std::unordered_map<std::string, std::vector<std::size_t>>;

JoinBuckets BuildJoinBuckets(const std::vector<Row>& right_rows,
                             std::size_t right_index) {
  JoinBuckets buckets;
  for (std::size_t i = 0; i < right_rows.size(); ++i) {
    buckets[JoinKey(right_rows[i][right_index])].push_back(i);
  }
  return buckets;
}

// nullopt when a predicate fails to evaluate (*failure holds the message);
// otherwise whether all hold, in order.
std::optional<bool> HoldsAll(const std::vector<const BoundExpr*>& predicates,
                             const Row& row, std::string* failure) {
  for (const BoundExpr* predicate : predicates) {
    std::optional<bool> holds =
        domain::EvaluateCondition(*predicate, row, failure);
    if (!holds) {
      return std::nullopt;
    }
    if (!*holds) {
      return false;
    }
  }
  return true;
}

// Appends the pairs that satisfy spec to *joined in nested-loop order: left
// rows in order, and for each the matching right rows in right order. A
// hash join probes only the bucket of the left key; otherwise every right row
// is a candidate. A pair is copied into *joined only after all residuals hold,
// so memory grows with the output, not with the candidates.
std::optional<ExecutionError> JoinRows(const RowSet& left, const RowSet& right,
                                       const JoinSpec& spec,
                                       std::vector<Row>* joined) {
  const std::size_t left_width = left.columns.size();
  JoinBuckets buckets;
  std::vector<std::size_t> every_position;
  if (spec.equi_columns) {
    buckets = BuildJoinBuckets(right.rows, spec.equi_columns->right_index);
  } else {
    every_position.resize(right.rows.size());
    std::iota(every_position.begin(), every_position.end(), std::size_t{0});
  }
  const std::vector<std::size_t> no_positions;
  const bool has_residuals = !spec.residuals.empty();
  // Scratch row the residuals are evaluated on; its left half is rewritten
  // once per left row and its right half once per candidate.
  Row combined(has_residuals ? left_width + right.columns.size() : 0);

  for (const Row& left_row : left.rows) {
    const std::vector<std::size_t>* candidates = &every_position;
    if (spec.equi_columns) {
      auto bucket =
          buckets.find(JoinKey(left_row[spec.equi_columns->left_index]));
      candidates = (bucket == buckets.end()) ? &no_positions : &bucket->second;
    }
    if (has_residuals) {
      std::ranges::copy(left_row, combined.begin());
    }
    for (std::size_t position : *candidates) {
      const Row& right_row = right.rows[position];
      // Two large INTs can share a double key, so confirm the bucket hit.
      if (spec.equi_columns &&
          !domain::ValuesEqual(left_row[spec.equi_columns->left_index],
                               right_row[spec.equi_columns->right_index])) {
        continue;
      }
      if (!has_residuals) {
        joined->push_back(Concatenate(left_row, right_row));
        continue;
      }
      std::ranges::copy(right_row, combined.begin() +
                                       static_cast<std::ptrdiff_t>(left_width));
      std::string failure;
      std::optional<bool> holds = HoldsAll(spec.residuals, combined, &failure);
      if (!holds) {
        return EvaluationError(std::move(failure));
      }
      if (*holds) {
        joined->push_back(combined);
      }
    }
  }
  return std::nullopt;
}

// fused_filter is the predicate of a FilterNode directly above the join, or
// null. It is borrowed from the plan for the duration of the call.
RowSetResult ExecuteJoin(const domain::JoinNode& join,
                         const BoundExpr* fused_filter,
                         const TableStore& store) {
  RowSet left;
  if (auto error = ExecuteChild(*join.left, store, &left)) {
    return std::move(*error);
  }
  RowSet right;
  if (auto error = ExecuteChild(*join.right, store, &right)) {
    return std::move(*error);
  }

  JoinSpec spec = MakeJoinSpec(join.condition.get(), fused_filter,
                               left.columns.size(), right.columns.size());
  RowSet output;
  output.columns = left.columns;
  output.columns.insert(output.columns.end(), right.columns.begin(),
                        right.columns.end());
  if (auto error = JoinRows(left, right, spec, &output.rows)) {
    return std::move(*error);
  }
  return output;
}

struct Group {
  Row keys;
  std::vector<domain::AggregateAccumulator> accumulators;
};

std::vector<domain::AggregateAccumulator> MakeAccumulators(
    const domain::AggregateNode& aggregate) {
  std::vector<domain::AggregateAccumulator> accumulators;
  for (const domain::AggregateSpec& spec : aggregate.aggregates) {
    // COUNT(*) has no argument; the type is only read for SUM.
    DataType argument_type =
        spec.arg ? DataTypeOf(spec.arg->type) : DataType::kInt;
    accumulators.emplace_back(spec.fn, argument_type);
  }
  return accumulators;
}

// Fails when the argument cannot be evaluated or a running sum overflows.
std::optional<ExecutionError> AddToAccumulator(
    const domain::AggregateSpec& spec, const Row& row,
    domain::AggregateAccumulator* accumulator) {
  if (!spec.arg) {
    accumulator->Add(Value(int64_t{0}));  // COUNT(*) cannot fail
    return std::nullopt;
  }
  std::string failure;
  std::optional<Value> argument = domain::Evaluate(*spec.arg, row, &failure);
  if (!argument) {
    return EvaluationError(std::move(failure));
  }
  if (!accumulator->Add(*argument)) {
    return OverflowError();
  }
  return std::nullopt;
}

std::vector<ColumnDef> AggregateColumns(
    const domain::AggregateNode& aggregate) {
  std::vector<ColumnDef> columns;
  for (std::size_t i = 0; i < aggregate.group_keys.size(); ++i) {
    columns.push_back(ColumnDef{"group_key_" + std::to_string(i),
                                DataTypeOf(aggregate.group_keys[i]->type)});
  }
  for (std::size_t i = 0; i < aggregate.aggregates.size(); ++i) {
    const domain::AggregateSpec& spec = aggregate.aggregates[i];
    DataType type = spec.arg ? DataTypeOf(spec.arg->type) : DataType::kInt;
    if (spec.fn == domain::AggregateFn::kCount) {
      type = DataType::kInt;
    }
    if (spec.fn == domain::AggregateFn::kAvg) {
      type = DataType::kDouble;
    }
    columns.push_back(ColumnDef{"aggregate_" + std::to_string(i), type});
  }
  return columns;
}

RowSetResult ExecuteAggregate(const domain::AggregateNode& aggregate,
                              const TableStore& store) {
  RowSet input;
  if (auto error = ExecuteChild(*aggregate.child, store, &input)) {
    return std::move(*error);
  }

  const std::vector<domain::AggregateAccumulator> prototypes =
      MakeAccumulators(aggregate);
  const bool has_group_keys = !aggregate.group_keys.empty();
  // Groups in first-seen order; group_positions maps an exact key to the
  // index in groups.
  std::vector<Group> groups;
  std::unordered_map<std::string, std::size_t> group_positions;
  // Without group keys the result is one row even when the input is empty.
  if (!has_group_keys) {
    groups.push_back(Group{{}, prototypes});
  }

  for (const Row& row : input.rows) {
    std::size_t position = 0;
    if (has_group_keys) {
      Row keys;
      std::string key_text;
      for (const BoundExprPtr& key_expr : aggregate.group_keys) {
        std::string failure;
        std::optional<Value> key = domain::Evaluate(*key_expr, row, &failure);
        if (!key) {
          return EvaluationError(std::move(failure));
        }
        AppendExactKey(*key, &key_text);
        keys.push_back(std::move(*key));
      }
      auto entry = group_positions.emplace(std::move(key_text), groups.size());
      if (entry.second) {
        groups.push_back(Group{std::move(keys), prototypes});
      }
      position = entry.first->second;
    }
    Group& group = groups[position];
    for (std::size_t i = 0; i < aggregate.aggregates.size(); ++i) {
      if (auto error = AddToAccumulator(aggregate.aggregates[i], row,
                                        &group.accumulators[i])) {
        return std::move(*error);
      }
    }
  }

  RowSet output{AggregateColumns(aggregate), {}};
  output.rows.reserve(groups.size());
  for (Group& group : groups) {
    // A group built from rows is never empty; only the single group of a query
    // without GROUP BY can have no rows, and AVG, MIN and MAX have no result
    // for it.
    for (std::size_t i = 0; i < group.accumulators.size(); ++i) {
      const domain::AggregateFn fn = aggregate.aggregates[i].fn;
      const bool has_no_result_when_empty = fn == domain::AggregateFn::kAvg ||
                                            fn == domain::AggregateFn::kMin ||
                                            fn == domain::AggregateFn::kMax;
      if (has_no_result_when_empty && group.accumulators[i].IsEmpty()) {
        return MakeError(ErrorKind::kRuntime,
                         std::string(AggregateFnName(fn)) + " over no rows");
      }
    }
    Row out_row = std::move(group.keys);
    for (const domain::AggregateAccumulator& accumulator : group.accumulators) {
      out_row.push_back(accumulator.Result());
    }
    output.rows.push_back(std::move(out_row));
  }
  return output;
}

RowSetResult ExecuteProject(const domain::ProjectNode& project,
                            const TableStore& store) {
  RowSet input;
  if (auto error = ExecuteChild(*project.child, store, &input)) {
    return std::move(*error);
  }

  RowSet output{project.output_columns, {}};
  output.rows.reserve(input.rows.size());
  for (const Row& row : input.rows) {
    Row out_row;
    out_row.reserve(project.exprs.size());
    for (const BoundExprPtr& expr : project.exprs) {
      std::string failure;
      std::optional<Value> value = domain::Evaluate(*expr, row, &failure);
      if (!value) {
        return EvaluationError(std::move(failure));
      }
      out_row.push_back(std::move(*value));
    }
    output.rows.push_back(std::move(out_row));
  }
  return output;
}

// Strict weak ordering over the sort keys; equal rows keep their input order
// because the caller uses std::stable_sort.
class RowOrder {
 public:
  explicit RowOrder(const std::vector<domain::SortKey>* keys) : keys_(keys) {}

  bool operator()(const Row& lhs, const Row& rhs) const {
    for (const domain::SortKey& key : *keys_) {
      int ordering =
          domain::CompareValues(lhs[key.column_index], rhs[key.column_index]);
      if (ordering == 0) {
        continue;
      }
      return key.is_descending ? ordering > 0 : ordering < 0;
    }
    return false;
  }

 private:
  const std::vector<domain::SortKey>* keys_;
};

RowSetResult ExecuteSort(const domain::SortNode& sort,
                         const TableStore& store) {
  RowSet output;
  if (auto error = ExecuteChild(*sort.child, store, &output)) {
    return std::move(*error);
  }

  for (const domain::SortKey& key : sort.keys) {
    assert(key.column_index < output.columns.size());
  }
  std::ranges::stable_sort(output.rows, RowOrder(&sort.keys));
  return output;
}

// Keeps the rows from position offset on, at most limit of them.
RowSetResult ExecuteLimit(const domain::LimitNode& limit,
                          const TableStore& store) {
  RowSet output;
  if (auto error = ExecuteChild(*limit.child, store, &output)) {
    return std::move(*error);
  }
  const std::size_t total = output.rows.size();
  // offset and limit are not negative (the parser accepts no sign).
  const std::size_t begin =
      std::min(total, static_cast<std::size_t>(limit.offset));
  std::size_t count = total - begin;
  if (limit.limit) {
    count = std::min(count, static_cast<std::size_t>(*limit.limit));
  }
  const auto first = output.rows.begin() + static_cast<std::ptrdiff_t>(begin);
  const auto last = first + static_cast<std::ptrdiff_t>(count);
  std::vector<Row> window(std::make_move_iterator(first),
                          std::make_move_iterator(last));
  output.rows = std::move(window);
  return output;
}

RowSetResult ExecuteNode(const PlanNode& plan, const TableStore& store) {
  if (const auto* scan = std::get_if<domain::ScanNode>(&plan.node)) {
    return ExecuteScan(*scan, store);
  }
  if (const auto* filter = std::get_if<domain::FilterNode>(&plan.node)) {
    return ExecuteFilter(*filter, store);
  }
  if (const auto* join = std::get_if<domain::JoinNode>(&plan.node)) {
    return ExecuteJoin(*join, nullptr, store);
  }
  if (const auto* aggregate = std::get_if<domain::AggregateNode>(&plan.node)) {
    return ExecuteAggregate(*aggregate, store);
  }
  if (const auto* project = std::get_if<domain::ProjectNode>(&plan.node)) {
    return ExecuteProject(*project, store);
  }
  if (const auto* sort = std::get_if<domain::SortNode>(&plan.node)) {
    return ExecuteSort(*sort, store);
  }
  if (const auto* limit = std::get_if<domain::LimitNode>(&plan.node)) {
    return ExecuteLimit(*limit, store);
  }
  return MakeError(ErrorKind::kUnsupported, "not a query plan node");
}

// ----------------------------------------------------------------------------
// Mutation planning.
// ----------------------------------------------------------------------------

using ChangedTables = std::map<std::string, std::vector<Row>>;

// Table.column in the declared spelling, as used in error messages.
std::string QualifiedName(const TableSchema& schema, std::size_t column_index) {
  return schema.name + "." + schema.columns[column_index].name;
}

ExecutionError DuplicateKeyError(const TableSchema& schema,
                                 std::size_t column_index, const Value& value) {
  return MakeError(ErrorKind::kConstraintViolation,
                   "duplicate key: " + QualifiedName(schema, column_index) +
                       "=" + domain::FormatValue(value));
}

ExecutionError NoMatchingRowError(const TableSchema& schema,
                                  const ForeignKey& foreign_key,
                                  const TableSchema& parent_schema,
                                  const Value& value) {
  return MakeError(
      ErrorKind::kConstraintViolation,
      "no matching row: " + QualifiedName(schema, foreign_key.column_index) +
          "=" + domain::FormatValue(value) + " not found in " +
          QualifiedName(parent_schema, foreign_key.ref_column_index));
}

// Check 1: a line break would split the row over two lines of the CSV file
// (ADR-0012).
std::optional<ExecutionError> CheckTextStorable(const TableSchema& schema,
                                                std::size_t column_index,
                                                const Value& value) {
  const auto* text = std::get_if<std::string>(&value);
  if (text == nullptr) {
    return std::nullopt;
  }
  if (text->find_first_of("\r\n") == std::string::npos) {
    return std::nullopt;
  }
  return MakeError(ErrorKind::kUnsupported,
                   "flatfile engine cannot store a line break in " +
                       QualifiedName(schema, column_index));
}

// One foreign key seen from the table it references. Both pointers borrow
// from the TableStore and are valid while the store is unchanged.
struct Reference {
  const TableData* child;
  const ForeignKey* foreign_key;
};

// Every foreign key that references table_key, in registration order of the
// child table and then declaration order, which is the order DELETE reports.
// The returned references borrow from store.
std::vector<Reference> FindReferences(const TableStore& store,
                                      const std::string& table_key) {
  std::vector<Reference> references;
  for (const std::string& name : store.order) {
    const TableData* child = store.Find(name);
    if (child == nullptr) {
      continue;
    }
    for (const ForeignKey& foreign_key : child->schema.foreign_keys) {
      if (domain::ToLower(foreign_key.ref_table) != table_key) {
        continue;
      }
      references.push_back(Reference{child, &foreign_key});
    }
  }
  return references;
}

bool ContainsValue(const std::vector<Row>& rows, std::size_t column_index,
                   const Value& value) {
  for (const Row& row : rows) {
    if (domain::ValuesEqual(row[column_index], value)) {
      return true;
    }
  }
  return false;
}

std::unordered_set<std::string> ColumnKeys(const std::vector<Row>& rows,
                                           std::size_t column_index) {
  std::unordered_set<std::string> keys;
  keys.reserve(rows.size());
  for (const Row& row : rows) {
    keys.insert(ExactKey(row[column_index]));
  }
  return keys;
}

// Stores in *matched the positions of the rows the predicate selects,
// ascending. A null predicate selects every row.
std::optional<ExecutionError> MatchRows(const BoundExpr* predicate,
                                        const std::vector<Row>& rows,
                                        std::vector<std::size_t>* matched) {
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (predicate != nullptr) {
      std::string failure;
      std::optional<bool> is_match =
          domain::EvaluateCondition(*predicate, rows[i], &failure);
      if (!is_match) {
        return EvaluationError(std::move(failure));
      }
      if (!*is_match) {
        continue;
      }
    }
    matched->push_back(i);
  }
  return std::nullopt;
}

MutationPlan NothingChanged(const std::string& table, std::size_t affected) {
  return MutationPlan{table, std::nullopt, {}, affected};
}

// ---- INSERT ----------------------------------------------------------------

std::optional<ExecutionError> CheckInsertForeignKeys(const TableStore& store,
                                                     const TableSchema& schema,
                                                     const Row& row) {
  for (const ForeignKey& foreign_key : schema.foreign_keys) {
    const TableData* parent = store.Find(foreign_key.ref_table);
    if (parent == nullptr) {
      return UnknownTableError(foreign_key.ref_table);
    }
    const Value& value = row[foreign_key.column_index];
    if (ContainsValue(parent->rows, foreign_key.ref_column_index, value)) {
      continue;
    }
    return NoMatchingRowError(schema, foreign_key, parent->schema, value);
  }
  return std::nullopt;
}

// The value an INSERT stores in the auto-increment column: one more than the
// largest value there, or 1 in an empty table (ADR-0016).
std::optional<ExecutionError> NextAutoIncrementValue(const TableData& table,
                                                     std::size_t column_index,
                                                     int64_t* next) {
  int64_t largest = 0;
  for (const Row& row : table.rows) {
    largest = std::max(largest, std::get<int64_t>(row[column_index]));
  }
  if (largest == std::numeric_limits<int64_t>::max()) {
    return OverflowError();
  }
  *next = largest + 1;
  return std::nullopt;
}

// The values already present in the unique columns (the primary key and every
// UNIQUE column), grown by each row of the statement that passes its checks,
// so that a row is also compared with the rows before it in the same batch.
// Lookup and insertion are O(1); building is O(n) per column.
class UniqueValues {
 public:
  explicit UniqueValues(const TableData& table) {
    const TableSchema& schema = table.schema;
    columns_.push_back(schema.primary_key_index);
    columns_.insert(columns_.end(), schema.unique_indexes.begin(),
                    schema.unique_indexes.end());
    for (std::size_t column_index : columns_) {
      keys_.push_back(ColumnKeys(table.rows, column_index));
    }
  }

  // The first unique column (primary key first, then ascending) whose value in
  // row is already present, or std::nullopt.
  std::optional<std::size_t> FindDuplicate(const Row& row) const {
    for (std::size_t i = 0; i < columns_.size(); ++i) {
      if (keys_[i].contains(ExactKey(row[columns_[i]]))) {
        return columns_[i];
      }
    }
    return std::nullopt;
  }

  void Add(const Row& row) {
    for (std::size_t i = 0; i < columns_.size(); ++i) {
      keys_[i].insert(ExactKey(row[columns_[i]]));
    }
  }

 private:
  std::vector<std::size_t> columns_;
  std::vector<std::unordered_set<std::string>>
      keys_;  // by position in columns_
};

// Precondition: every row of insert.rows has one value per column of the table,
// each of the column type (the InsertNode contract). The rows are checked in
// order, each by the checks of docs/DesignDocument.md section 2.5 in that
// order, and the first failure is returned.
MutationOutcome PlanInsert(const domain::InsertNode& insert,
                           const TableStore& store) {
  const TableData* target = store.Find(insert.table);
  if (target == nullptr) {
    return UnknownTableError(insert.table);
  }
  const TableSchema& schema = target->schema;
  std::vector<Row> rows = insert.rows;
  UniqueValues unique_values(*target);
  int64_t next_generated = 0;
  for (std::size_t row_number = 0; row_number < rows.size(); ++row_number) {
    Row& row = rows[row_number];
    assert(row.size() == schema.columns.size());
    if (insert.generated_index) {
      if (row_number == 0) {
        if (auto error = NextAutoIncrementValue(
                *target, *insert.generated_index, &next_generated)) {
          return std::move(*error);
        }
      } else if (next_generated == std::numeric_limits<int64_t>::max()) {
        return OverflowError();
      } else {
        ++next_generated;
      }
      row[*insert.generated_index] = Value(next_generated);
    }
    for (std::size_t i = 0; i < row.size(); ++i) {
      if (auto error = CheckTextStorable(schema, i, row[i])) {
        return std::move(*error);
      }
    }
    if (std::optional<std::size_t> column_index =
            unique_values.FindDuplicate(row)) {
      return DuplicateKeyError(schema, *column_index, row[*column_index]);
    }
    if (auto error = CheckInsertForeignKeys(store, schema, row)) {
      return std::move(*error);
    }
    unique_values.Add(row);
  }
  const std::size_t affected = rows.size();
  return MutationPlan{insert.table, std::move(rows), {}, affected};
}

// ---- UPDATE ----------------------------------------------------------------

// Evaluates every assignment against the old row and stores the coerced result
// in the matching position of *new_rows, which the caller initialized as a copy
// of old_rows.
std::optional<ExecutionError> ComputeNewRows(
    const domain::UpdateNode& update, const TableSchema& schema,
    const std::vector<Row>& old_rows, const std::vector<std::size_t>& matched,
    std::vector<Row>* new_rows) {
  for (std::size_t position : matched) {
    const Row& old_row = old_rows[position];
    for (const domain::BoundAssignment& assignment : update.assignments) {
      std::string failure;
      std::optional<Value> value =
          domain::Evaluate(*assignment.value, old_row, &failure);
      if (!value) {
        return EvaluationError(std::move(failure));
      }
      const ColumnDef& column = schema.columns[assignment.column_index];
      std::optional<Value> coerced = domain::CoerceValue(*value, column.type);
      if (!coerced) {
        return MakeError(ErrorKind::kBind,
                         "type mismatch assigning to " +
                             QualifiedName(schema, assignment.column_index));
      }
      (*new_rows)[position][assignment.column_index] = std::move(*coerced);
    }
  }
  return std::nullopt;
}

// The rows an UPDATE reads and produces. Borrows all three vectors, which
// outlive it. Every UPDATE check looks at changed cells only: a cell that keeps
// its value is already stored and was validated, or loaded, before.
struct UpdateRows {
  const std::vector<Row>& old_rows;
  const std::vector<Row>& new_rows;         // same shape as old_rows
  const std::vector<std::size_t>& matched;  // positions chosen by WHERE

  bool IsChanged(std::size_t position, std::size_t column_index) const {
    return old_rows[position][column_index] != new_rows[position][column_index];
  }
};

// Check 1 on the changed assigned values.
std::optional<ExecutionError> CheckChangedStorable(
    const domain::UpdateNode& update, const TableSchema& schema,
    const UpdateRows& rows) {
  for (std::size_t position : rows.matched) {
    for (const domain::BoundAssignment& assignment : update.assignments) {
      if (!rows.IsChanged(position, assignment.column_index)) {
        continue;
      }
      const Value& value = rows.new_rows[position][assignment.column_index];
      if (auto error =
              CheckTextStorable(schema, assignment.column_index, value)) {
        return error;
      }
    }
  }
  return std::nullopt;
}

// Assigned columns that some foreign key references, ascending.
std::set<std::size_t> AssignedReferencedColumns(
    const domain::UpdateNode& update,
    const std::vector<Reference>& references) {
  std::set<std::size_t> referenced;
  for (const Reference& reference : references) {
    referenced.insert(reference.foreign_key->ref_column_index);
  }
  std::set<std::size_t> assigned_referenced;
  for (const domain::BoundAssignment& assignment : update.assignments) {
    if (referenced.contains(assignment.column_index)) {
      assigned_referenced.insert(assignment.column_index);
    }
  }
  return assigned_referenced;
}

// Assigned columns that must stay unique: the UNIQUE columns and the columns
// some foreign key references (which are unique by declaration), ascending.
std::set<std::size_t> AssignedUniqueColumns(
    const domain::UpdateNode& update, const TableSchema& schema,
    const std::set<std::size_t>& assigned_referenced) {
  std::set<std::size_t> unique_columns = assigned_referenced;
  for (const domain::BoundAssignment& assignment : update.assignments) {
    if (std::ranges::find(schema.unique_indexes, assignment.column_index) !=
        schema.unique_indexes.end()) {
      unique_columns.insert(assignment.column_index);
    }
  }
  return unique_columns;
}

// Check 3 on the final state of the table: a changed cell must not end with a
// value that another row also holds. Swapping two unique values is therefore
// legal, and a duplicate that the update did not touch is not reported.
std::optional<ExecutionError> CheckChangedUniqueness(
    const TableSchema& schema, const std::set<std::size_t>& columns,
    const UpdateRows& rows) {
  for (std::size_t column_index : columns) {
    std::unordered_map<std::string, std::size_t> occurrences;
    occurrences.reserve(rows.new_rows.size());
    for (const Row& row : rows.new_rows) {
      ++occurrences[ExactKey(row[column_index])];
    }
    for (std::size_t position : rows.matched) {
      if (!rows.IsChanged(position, column_index)) {
        continue;
      }
      const Value& value = rows.new_rows[position][column_index];
      if (occurrences[ExactKey(value)] > 1) {
        return DuplicateKeyError(schema, column_index, value);
      }
    }
  }
  return std::nullopt;
}

bool IsAssigned(const domain::UpdateNode& update, std::size_t column_index) {
  for (const domain::BoundAssignment& assignment : update.assignments) {
    if (assignment.column_index == column_index) {
      return true;
    }
  }
  return false;
}

// Check 5 for every foreign-key column whose value the update changed, in
// declaration order.
std::optional<ExecutionError> CheckChangedForeignKeys(
    const TableStore& store, const TableData& target,
    const domain::UpdateNode& update, const UpdateRows& rows) {
  const TableSchema& schema = target.schema;
  for (const ForeignKey& foreign_key : schema.foreign_keys) {
    if (!IsAssigned(update, foreign_key.column_index)) {
      continue;
    }
    std::vector<std::size_t> changed_positions;
    for (std::size_t position : rows.matched) {
      if (rows.IsChanged(position, foreign_key.column_index)) {
        changed_positions.push_back(position);
      }
    }
    if (changed_positions.empty()) {
      continue;
    }

    const TableData* parent = store.Find(foreign_key.ref_table);
    if (parent == nullptr) {
      return UnknownTableError(foreign_key.ref_table);
    }
    // A self-referencing key is checked against the rows after the update.
    const std::vector<Row>& parent_rows =
        (parent == &target) ? rows.new_rows : parent->rows;
    std::unordered_set<std::string> parent_keys =
        ColumnKeys(parent_rows, foreign_key.ref_column_index);
    for (std::size_t position : changed_positions) {
      const Value& value = rows.new_rows[position][foreign_key.column_index];
      if (parent_keys.contains(ExactKey(value))) {
        continue;
      }
      return NoMatchingRowError(schema, foreign_key, parent->schema, value);
    }
  }
  return std::nullopt;
}

// Old value to new value for the rows whose value in the column changed. If an
// old value occurs in several rows with different new values, the first wins.
std::unordered_map<std::string, Value> BuildRenames(
    const std::vector<Row>& old_rows, const std::vector<Row>& new_rows,
    const std::vector<std::size_t>& matched, std::size_t column_index) {
  std::unordered_map<std::string, Value> renames;
  for (std::size_t position : matched) {
    const Value& old_value = old_rows[position][column_index];
    const Value& new_value = new_rows[position][column_index];
    if (old_value == new_value) {
      continue;
    }
    renames.emplace(ExactKey(old_value), new_value);
  }
  return renames;
}

// Rewrites each cell of the column once through renames. A cell is looked up a
// single time, so A to B and B to C in one map never chains A to C.
bool RewriteColumn(std::vector<Row>* rows, std::size_t column_index,
                   const std::unordered_map<std::string, Value>& renames) {
  bool is_changed = false;
  for (Row& row : *rows) {
    auto rename = renames.find(ExactKey(row[column_index]));
    if (rename == renames.end()) {
      continue;
    }
    row[column_index] = rename->second;
    is_changed = true;
  }
  return is_changed;
}

// Rewrites the child table of one reference and records it in changed when at
// least one row differs afterwards.
void CascadeInto(const Reference& reference,
                 const std::unordered_map<std::string, Value>& renames,
                 ChangedTables* changed) {
  std::string child_key = domain::ToLower(reference.child->schema.name);
  auto staged = changed->find(child_key);
  bool is_newly_staged = (staged == changed->end());
  if (is_newly_staged) {
    staged = changed->emplace(child_key, reference.child->rows).first;
  }
  bool is_changed = RewriteColumn(&staged->second,
                                  reference.foreign_key->column_index, renames);
  if (is_newly_staged && !is_changed) {
    changed->erase(staged);
  }
}

MutationOutcome PlanUpdate(const domain::UpdateNode& update,
                           const TableStore& store) {
  const TableData* target = store.Find(update.table);
  if (target == nullptr) {
    return UnknownTableError(update.table);
  }
  const TableSchema& schema = target->schema;

  std::vector<std::size_t> matched;
  if (auto error = MatchRows(update.predicate.get(), target->rows, &matched)) {
    return std::move(*error);
  }
  if (matched.empty()) {
    return NothingChanged(update.table, 0);
  }

  std::vector<Row> new_rows = target->rows;
  if (auto error =
          ComputeNewRows(update, schema, target->rows, matched, &new_rows)) {
    return std::move(*error);
  }
  const UpdateRows rows{target->rows, new_rows, matched};
  if (auto error = CheckChangedStorable(update, schema, rows)) {
    return std::move(*error);
  }

  std::string target_key = domain::ToLower(schema.name);
  std::vector<Reference> references = FindReferences(store, target_key);
  std::set<std::size_t> referenced_columns =
      AssignedReferencedColumns(update, references);
  std::set<std::size_t> unique_columns =
      AssignedUniqueColumns(update, schema, referenced_columns);
  if (auto error = CheckChangedUniqueness(schema, unique_columns, rows)) {
    return std::move(*error);
  }
  if (auto error = CheckChangedForeignKeys(store, *target, update, rows)) {
    return std::move(*error);
  }

  // If no row changed there is nothing to write and nothing to cascade.
  if (new_rows == target->rows) {
    return NothingChanged(update.table, matched.size());
  }

  // The target goes in first so that a self-referencing child is rewritten on
  // top of the updated rows.
  ChangedTables changed;
  changed[target_key] = std::move(new_rows);
  const std::vector<Row>& updated_rows = changed[target_key];
  for (std::size_t column_index : referenced_columns) {
    auto renames =
        BuildRenames(target->rows, updated_rows, matched, column_index);
    if (renames.empty()) {
      continue;
    }
    for (const Reference& reference : references) {
      if (reference.foreign_key->ref_column_index != column_index) {
        continue;
      }
      CascadeInto(reference, renames, &changed);
    }
  }
  return MutationPlan{update.table, std::nullopt, std::move(changed),
                      matched.size()};
}

// ---- DELETE ----------------------------------------------------------------

// The values a child table holds in one foreign-key column.
struct ReferencedValues {
  Reference reference;
  std::unordered_set<std::string> values;
};

MutationOutcome PlanDelete(const domain::DeleteNode& delete_node,
                           const TableStore& store) {
  const TableData* target = store.Find(delete_node.table);
  if (target == nullptr) {
    return UnknownTableError(delete_node.table);
  }
  const TableSchema& schema = target->schema;

  std::vector<std::size_t> matched;
  if (auto error =
          MatchRows(delete_node.predicate.get(), target->rows, &matched)) {
    return std::move(*error);
  }
  if (matched.empty()) {
    return NothingChanged(delete_node.table, 0);
  }

  // One set per child table and foreign key, built once for the statement.
  std::vector<ReferencedValues> referenced_values;
  for (const Reference& reference :
       FindReferences(store, domain::ToLower(schema.name))) {
    referenced_values.push_back(ReferencedValues{
        reference, ColumnKeys(reference.child->rows,
                              reference.foreign_key->column_index)});
  }

  // Rows in table order, then child tables in registration order: the first
  // hit is the one reported.
  for (std::size_t position : matched) {
    const Row& row = target->rows[position];
    for (const ReferencedValues& entry : referenced_values) {
      const ForeignKey& foreign_key = *entry.reference.foreign_key;
      if (!entry.values.contains(ExactKey(row[foreign_key.ref_column_index]))) {
        continue;
      }
      return MakeError(ErrorKind::kConstraintViolation,
                       QualifiedName(schema, schema.primary_key_index) + "=" +
                           domain::FormatValue(row[schema.primary_key_index]) +
                           " is referenced by " +
                           QualifiedName(entry.reference.child->schema,
                                         foreign_key.column_index));
    }
  }

  std::vector<Row> remaining;
  remaining.reserve(target->rows.size() - matched.size());
  std::size_t next_match = 0;
  for (std::size_t i = 0; i < target->rows.size(); ++i) {
    if (next_match < matched.size() && matched[next_match] == i) {
      ++next_match;
      continue;
    }
    remaining.push_back(target->rows[i]);
  }
  ChangedTables changed;
  changed[domain::ToLower(schema.name)] = std::move(remaining);
  return MutationPlan{delete_node.table, std::nullopt, std::move(changed),
                      matched.size()};
}

// ---- DDL -------------------------------------------------------------------

ExecutionError TableExistsError(const std::string& name) {
  return MakeError(ErrorKind::kBind, "table " + name + " already exists");
}

// The foreign-key and uniqueness rules of an adopted CSV, in the order the
// engine applies them to an INSERT: primary key, UNIQUE columns, then foreign
// keys. The first violation is reported with the file it belongs to.
std::optional<ExecutionError> CheckAdoptedRows(const TableSchema& schema,
                                               const std::vector<Row>& rows,
                                               const TableStore& store) {
  const std::string prefix =
      "cannot adopt " + domain::ToLower(schema.name) + ".csv: ";
  std::vector<std::size_t> unique_columns = {schema.primary_key_index};
  unique_columns.insert(unique_columns.end(), schema.unique_indexes.begin(),
                        schema.unique_indexes.end());
  for (std::size_t column_index : unique_columns) {
    std::unordered_set<std::string> seen;
    seen.reserve(rows.size());
    for (const Row& row : rows) {
      if (seen.insert(ExactKey(row[column_index])).second) {
        continue;
      }
      ExecutionError error =
          DuplicateKeyError(schema, column_index, row[column_index]);
      error.message = prefix + error.message;
      return error;
    }
  }
  for (const ForeignKey& foreign_key : schema.foreign_keys) {
    const TableData* parent = store.Find(foreign_key.ref_table);
    if (parent == nullptr) {
      return UnknownTableError(foreign_key.ref_table);
    }
    std::unordered_set<std::string> parent_keys =
        ColumnKeys(parent->rows, foreign_key.ref_column_index);
    for (const Row& row : rows) {
      const Value& value = row[foreign_key.column_index];
      if (parent_keys.contains(ExactKey(value))) {
        continue;
      }
      ExecutionError error =
          NoMatchingRowError(schema, foreign_key, parent->schema, value);
      error.message = prefix + error.message;
      return error;
    }
  }
  return std::nullopt;
}

SchemaChangeOutcome PlanCreate(
    const domain::CreateTableNode& create, const TableStore& store,
    const std::optional<std::vector<Row>>& existing_rows) {
  const TableSchema& schema = create.schema;
  if (store.Find(schema.name) != nullptr) {
    return TableExistsError(schema.name);
  }
  std::vector<Row> rows;
  std::string tag = "CREATE TABLE " + schema.name;
  if (existing_rows) {
    if (auto error = CheckAdoptedRows(schema, *existing_rows, store)) {
      return std::move(*error);
    }
    rows = *existing_rows;
    tag += " (adopted " + std::to_string(rows.size()) + " rows)";
  }
  return SchemaChange{
      SchemaChangeKind::kCreate, std::move(tag), domain::ToLower(schema.name),
      TableData{schema, std::move(rows)}, existing_rows.has_value()};
}

// A new table that arrives with its rows (`.import`). The Domain has checked
// the table name against the catalog and the key for duplicates; both are
// checked again, because the plan is not trusted to have been built by it.
SchemaChangeOutcome PlanImport(const domain::ImportTableNode& import,
                               const TableStore& store) {
  const TableSchema& schema = import.schema;
  if (store.Find(schema.name) != nullptr) {
    return TableExistsError(schema.name);
  }
  std::unordered_set<std::string> keys;
  for (const Row& row : import.rows) {
    assert(row.size() == schema.columns.size());
    for (std::size_t i = 0; i < row.size(); ++i) {
      if (auto error = CheckTextStorable(schema, i, row[i])) {
        return std::move(*error);
      }
    }
    const std::size_t key_index = schema.primary_key_index;
    if (!keys.insert(ExactKey(row[key_index])).second) {
      return DuplicateKeyError(schema, key_index, row[key_index]);
    }
  }
  std::string tag = "IMPORT " + schema.name + " (imported " +
                    std::to_string(import.rows.size()) + " rows)";
  return SchemaChange{SchemaChangeKind::kCreate, std::move(tag),
                      domain::ToLower(schema.name),
                      TableData{schema, import.rows}, false};
}

SchemaChangeOutcome PlanAddColumn(const domain::AddColumnNode& add_column,
                                  const TableStore& store) {
  const TableData* target = store.Find(add_column.table);
  if (target == nullptr) {
    return UnknownTableError(add_column.table);
  }
  assert(add_column.column.default_value.has_value());
  TableData changed = *target;
  changed.schema.columns.push_back(add_column.column);
  const std::size_t column_index = changed.schema.columns.size() - 1;
  const Value& default_value = *add_column.column.default_value;
  if (auto error =
          CheckTextStorable(changed.schema, column_index, default_value)) {
    return std::move(*error);
  }
  for (Row& row : changed.rows) {
    row.push_back(default_value);
  }
  return SchemaChange{
      SchemaChangeKind::kAddColumn, "ALTER TABLE " + target->schema.name,
      domain::ToLower(target->schema.name), std::move(changed), false};
}

SchemaChangeOutcome PlanDrop(const domain::DropTableNode& drop,
                             const TableStore& store) {
  const TableData* target = store.Find(drop.table);
  if (target == nullptr) {
    return UnknownTableError(drop.table);
  }
  const std::string key = domain::ToLower(target->schema.name);
  for (const Reference& reference : FindReferences(store, key)) {
    if (domain::ToLower(reference.child->schema.name) == key) {
      continue;
    }
    return MakeError(ErrorKind::kConstraintViolation,
                     "table " + target->schema.name + " is referenced by " +
                         QualifiedName(reference.child->schema,
                                       reference.foreign_key->column_index));
  }
  return SchemaChange{SchemaChangeKind::kDrop,
                      "DROP TABLE " + target->schema.name, key, std::nullopt,
                      false};
}

}  // namespace

domain::ExecutionResult RunQuery(const domain::PlanNode& plan,
                                 const TableStore& store) {
  RowSetResult result = ExecuteNode(plan, store);
  if (auto* error = std::get_if<ExecutionError>(&result)) {
    return std::move(*error);
  }
  // The error alternative was ruled out above.
  RowSet& row_set = std::get<RowSet>(result);
  return domain::ResultSet{std::move(row_set.columns), std::move(row_set.rows)};
}

MutationOutcome PlanMutation(const domain::PlanNode& plan,
                             const TableStore& store) {
  if (const auto* insert = std::get_if<domain::InsertNode>(&plan.node)) {
    return PlanInsert(*insert, store);
  }
  if (const auto* update = std::get_if<domain::UpdateNode>(&plan.node)) {
    return PlanUpdate(*update, store);
  }
  if (const auto* delete_node = std::get_if<domain::DeleteNode>(&plan.node)) {
    return PlanDelete(*delete_node, store);
  }
  return MakeError(ErrorKind::kUnsupported, "not a write plan node");
}

SchemaChangeOutcome PlanSchemaChange(
    const domain::PlanNode& plan, const TableStore& store,
    const std::optional<std::vector<Row>>& existing_rows) {
  if (const auto* create = std::get_if<domain::CreateTableNode>(&plan.node)) {
    return PlanCreate(*create, store, existing_rows);
  }
  if (const auto* add_column = std::get_if<domain::AddColumnNode>(&plan.node)) {
    return PlanAddColumn(*add_column, store);
  }
  if (const auto* drop = std::get_if<domain::DropTableNode>(&plan.node)) {
    return PlanDrop(*drop, store);
  }
  if (const auto* import = std::get_if<domain::ImportTableNode>(&plan.node)) {
    return PlanImport(*import, store);
  }
  return MakeError(ErrorKind::kUnsupported, "not a schema change node");
}

}  // namespace flatfiledb::infrastructure
