#ifndef FLATFILEDB_DOMAIN_EXPRESSION_H_
#define FLATFILEDB_DOMAIN_EXPRESSION_H_

#include <cstdint>
#include <optional>

#include "flatfiledb/domain/ast.h"
#include "flatfiledb/domain/plan.h"
#include "flatfiledb/domain/value.h"

namespace flatfiledb::domain {

// Evaluates a non-boolean bound expression against row. Returns std::nullopt
// when an INT operation overflows 64 bits or a DOUBLE result is not finite.
// Precondition: expr.type is not kBool, expr contains no BoundAggregate, and
// every BoundColumn index is inside row.
std::optional<Value> Evaluate(const BoundExpr& expr, const Row& row);

// Evaluates a kBool expression. Returns std::nullopt on numeric overflow in an
// operand. Same preconditions as Evaluate, with expr.type equal to kBool.
std::optional<bool> EvaluateCondition(const BoundExpr& expr, const Row& row);

// Accumulates COUNT or SUM over a group. SUM keeps the argument type.
class AggregateAccumulator {
 public:
  AggregateAccumulator(AggregateFn fn, DataType argument_type);

  // Adds one input row. For COUNT(*), pass any value. Returns false when an
  // INT sum overflows 64 bits or a DOUBLE sum is not finite; the accumulator
  // is then unusable.
  bool Add(const Value& value);

  // COUNT: INT row count. SUM: sum of the added values, or the zero of the
  // argument type when nothing was added.
  Value Result() const;

 private:
  AggregateFn fn_;
  DataType argument_type_;
  int64_t count_ = 0;
  int64_t int_sum_ = 0;
  double double_sum_ = 0.0;
};

}  // namespace flatfiledb::domain

#endif  // FLATFILEDB_DOMAIN_EXPRESSION_H_
