#ifndef FLATFILEDB_DOMAIN_EXPRESSION_H_
#define FLATFILEDB_DOMAIN_EXPRESSION_H_

#include <cstdint>
#include <optional>
#include <string>

#include "flatfiledb/domain/ast.h"
#include "flatfiledb/domain/plan.h"
#include "flatfiledb/domain/value.h"

namespace flatfiledb::domain {

// Evaluates a non-boolean bound expression against row. Returns std::nullopt
// on a runtime failure: an INT operation that overflows 64 bits, a DOUBLE
// result that is not finite, a division by zero, or a function failure (see
// ApplyScalarFunction). When failure is not null, a failure writes its kRuntime
// message there ("numeric overflow", "division by zero", ...); success leaves
// it untouched.
// Precondition: expr.type is not kBool, expr contains no BoundAggregate, and
// every BoundColumn index is inside row.
std::optional<Value> Evaluate(const BoundExpr& expr, const Row& row,
                              std::string* failure = nullptr);

// Evaluates a kBool expression. Returns std::nullopt on a runtime failure in an
// operand; failure is written as for Evaluate. Same preconditions as Evaluate,
// with expr.type equal to kBool.
std::optional<bool> EvaluateCondition(const BoundExpr& expr, const Row& row,
                                      std::string* failure = nullptr);

// Accumulates COUNT, SUM, AVG, MIN or MAX over a group. SUM keeps the argument
// type, AVG is a DOUBLE, MIN and MAX keep the argument type.
class AggregateAccumulator {
 public:
  AggregateAccumulator(AggregateFn fn, DataType argument_type);

  // Adds one input row. For COUNT(*), pass any value. Returns false when an
  // INT sum overflows 64 bits or a DOUBLE sum is not finite; the accumulator
  // is then unusable.
  bool Add(const Value& value);

  // True until the first Add. AVG, MIN and MAX have no result while empty; the
  // caller reports "<FN> over no rows".
  bool IsEmpty() const { return count_ == 0; }

  // COUNT: INT row count. SUM: sum of the added values, or the zero of the
  // argument type when nothing was added. AVG: the mean, as a DOUBLE. MIN and
  // MAX: the smallest or largest added value. While empty, AVG, MIN and MAX
  // return the zero of their type; the caller checks IsEmpty() first.
  Value Result() const;

 private:
  AggregateFn fn_;
  DataType argument_type_;
  int64_t count_ = 0;
  int64_t int_sum_ = 0;
  double double_sum_ = 0.0;
  std::optional<Value> extreme_;  // MIN and MAX
};

}  // namespace flatfiledb::domain

#endif  // FLATFILEDB_DOMAIN_EXPRESSION_H_
