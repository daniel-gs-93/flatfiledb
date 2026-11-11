#include "flatfiledb/domain/expression.h"

#include <cassert>
#include <cmath>

namespace flatfiledb::domain {

namespace {

// INT with INT stays INT and can overflow; any DOUBLE operand promotes the
// result to DOUBLE.
std::optional<Value> Arithmetic(BinaryOp op, const Value& lhs,
                                const Value& rhs) {
  const auto* lhs_int = std::get_if<int64_t>(&lhs);
  const auto* rhs_int = std::get_if<int64_t>(&rhs);
  if (lhs_int != nullptr && rhs_int != nullptr) {
    int64_t result = 0;
    bool is_overflow =
        (op == BinaryOp::kAdd)
            ? __builtin_add_overflow(*lhs_int, *rhs_int, &result)
            : __builtin_sub_overflow(*lhs_int, *rhs_int, &result);
    if (is_overflow) {
      return std::nullopt;
    }
    return Value(result);
  }
  double lhs_double = ToDouble(lhs);
  double rhs_double = ToDouble(rhs);
  double result =
      op == BinaryOp::kAdd ? lhs_double + rhs_double : lhs_double - rhs_double;
  // An infinite or NaN result would print and compare inconsistently.
  if (!std::isfinite(result)) {
    return std::nullopt;
  }
  return Value(result);
}

bool CompareWith(BinaryOp op, int ordering) {
  switch (op) {
    case BinaryOp::kEq:
      return ordering == 0;
    case BinaryOp::kNe:
      return ordering != 0;
    case BinaryOp::kLt:
      return ordering < 0;
    case BinaryOp::kLe:
      return ordering <= 0;
    case BinaryOp::kGt:
      return ordering > 0;
    case BinaryOp::kGe:
      return ordering >= 0;
    default:
      break;
  }
  assert(false && "not a comparison operator");
  return false;
}

}  // namespace

std::optional<Value> Evaluate(const BoundExpr& expr, const Row& row) {
  assert(expr.type != ExprType::kBool);
  if (const auto* literal = std::get_if<BoundLiteral>(&expr.node)) {
    return literal->value;
  }
  if (const auto* column = std::get_if<BoundColumn>(&expr.node)) {
    assert(column->index < row.size());
    return row[column->index];
  }
  const auto* binary = std::get_if<BoundBinary>(&expr.node);
  assert(binary != nullptr && "aggregates are rewritten before evaluation");
  assert(binary->op == BinaryOp::kAdd || binary->op == BinaryOp::kSub);
  std::optional<Value> lhs = Evaluate(*binary->lhs, row);
  if (!lhs) {
    return std::nullopt;
  }
  std::optional<Value> rhs = Evaluate(*binary->rhs, row);
  if (!rhs) {
    return std::nullopt;
  }
  return Arithmetic(binary->op, *lhs, *rhs);
}

std::optional<bool> EvaluateCondition(const BoundExpr& expr, const Row& row) {
  assert(expr.type == ExprType::kBool);
  const auto* binary = std::get_if<BoundBinary>(&expr.node);
  assert(binary != nullptr);
  if (binary->op == BinaryOp::kAnd || binary->op == BinaryOp::kOr) {
    std::optional<bool> lhs = EvaluateCondition(*binary->lhs, row);
    if (!lhs) {
      return std::nullopt;
    }
    if (binary->op == BinaryOp::kAnd && !*lhs) {
      return false;
    }
    if (binary->op == BinaryOp::kOr && *lhs) {
      return true;
    }
    return EvaluateCondition(*binary->rhs, row);
  }
  std::optional<Value> lhs = Evaluate(*binary->lhs, row);
  if (!lhs) {
    return std::nullopt;
  }
  std::optional<Value> rhs = Evaluate(*binary->rhs, row);
  if (!rhs) {
    return std::nullopt;
  }
  return CompareWith(binary->op, CompareValues(*lhs, *rhs));
}

AggregateAccumulator::AggregateAccumulator(AggregateFn fn,
                                           DataType argument_type)
    : fn_(fn), argument_type_(argument_type) {}

bool AggregateAccumulator::Add(const Value& value) {
  ++count_;
  if (fn_ == AggregateFn::kCount) {
    return true;
  }
  assert(argument_type_ != DataType::kText);
  if (argument_type_ == DataType::kDouble) {
    double_sum_ += ToDouble(value);
    return std::isfinite(double_sum_);
  }
  return !__builtin_add_overflow(int_sum_, std::get<int64_t>(value), &int_sum_);
}

Value AggregateAccumulator::Result() const {
  if (fn_ == AggregateFn::kCount) {
    return Value(count_);
  }
  if (argument_type_ == DataType::kDouble) {
    return Value(double_sum_);
  }
  return Value(int_sum_);
}

}  // namespace flatfiledb::domain
