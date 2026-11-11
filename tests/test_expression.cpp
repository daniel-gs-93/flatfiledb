#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>

#include "flatfiledb/domain/expression.h"
#include "test_harness.h"

namespace {

namespace domain = flatfiledb::domain;

constexpr int64_t kInt64Max = std::numeric_limits<int64_t>::max();
constexpr int64_t kInt64Min = std::numeric_limits<int64_t>::min();

domain::BoundExprPtr MakeLiteral(domain::ExprType type, domain::Value value) {
  return std::make_unique<domain::BoundExpr>(
      domain::BoundExpr{type, domain::BoundLiteral{std::move(value)}});
}

domain::BoundExprPtr IntLiteral(int64_t value) {
  return MakeLiteral(domain::ExprType::kInt, domain::Value(value));
}

domain::BoundExprPtr DoubleLiteral(double value) {
  return MakeLiteral(domain::ExprType::kDouble, domain::Value(value));
}

domain::BoundExprPtr TextLiteral(const std::string& value) {
  return MakeLiteral(domain::ExprType::kText, domain::Value(value));
}

domain::ExprType ResultTypeOf(domain::BinaryOp op, const domain::BoundExpr& lhs,
                              const domain::BoundExpr& rhs) {
  if (op != domain::BinaryOp::kAdd && op != domain::BinaryOp::kSub) {
    return domain::ExprType::kBool;
  }
  bool has_double = lhs.type == domain::ExprType::kDouble ||
                    rhs.type == domain::ExprType::kDouble;
  return has_double ? domain::ExprType::kDouble : domain::ExprType::kInt;
}

domain::BoundExprPtr Binary(domain::BinaryOp op, domain::BoundExprPtr lhs,
                            domain::BoundExprPtr rhs) {
  domain::ExprType type = ResultTypeOf(op, *lhs, *rhs);
  return std::make_unique<domain::BoundExpr>(domain::BoundExpr{
      type, domain::BoundBinary{op, std::move(lhs), std::move(rhs)}});
}

// `1 = 1` is true, `1 = 2` is false: the smallest kBool operands.
domain::BoundExprPtr Boolean(bool is_true) {
  return Binary(domain::BinaryOp::kEq, IntLiteral(1),
                IntLiteral(is_true ? 1 : 2));
}

bool IsTrue(const domain::BoundExpr& condition) {
  std::optional<bool> result =
      domain::EvaluateCondition(condition, domain::Row{});
  return result.has_value() && *result;
}

bool IsFalse(const domain::BoundExpr& condition) {
  std::optional<bool> result =
      domain::EvaluateCondition(condition, domain::Row{});
  return result.has_value() && !*result;
}

bool IsInt(const std::optional<domain::Value>& value, int64_t expected) {
  return value.has_value() && std::holds_alternative<int64_t>(*value) &&
         std::get<int64_t>(*value) == expected;
}

bool IsDouble(const std::optional<domain::Value>& value, double expected) {
  return value.has_value() && std::holds_alternative<double>(*value) &&
         std::get<double>(*value) == expected;
}

bool IsInt(const domain::Value& value, int64_t expected) {
  return std::holds_alternative<int64_t>(value) &&
         std::get<int64_t>(value) == expected;
}

bool IsDouble(const domain::Value& value, double expected) {
  return std::holds_alternative<double>(value) &&
         std::get<double>(value) == expected;
}

}  // namespace

TEST(test_expression_numeric_comparison_mixes_int_and_double) {
  CHECK(IsTrue(*Binary(domain::BinaryOp::kLt, IntLiteral(1), IntLiteral(2))));
  CHECK(IsTrue(
      *Binary(domain::BinaryOp::kGt, DoubleLiteral(2.5), IntLiteral(2))));
  CHECK(IsTrue(
      *Binary(domain::BinaryOp::kEq, IntLiteral(2), DoubleLiteral(2.0))));
  // 2^53 + 1 has no DOUBLE representation, so the conversion makes the two
  // equal.
  CHECK(IsTrue(*Binary(domain::BinaryOp::kEq, IntLiteral(9007199254740993),
                       DoubleLiteral(9007199254740992.0))));
}

TEST(test_expression_text_comparison_uses_byte_order) {
  CHECK(IsTrue(
      *Binary(domain::BinaryOp::kLt, TextLiteral("a"), TextLiteral("b"))));
  CHECK(IsTrue(
      *Binary(domain::BinaryOp::kLt, TextLiteral("B"), TextLiteral("a"))));
  CHECK(IsTrue(
      *Binary(domain::BinaryOp::kLt, TextLiteral(""), TextLiteral("a"))));
}

TEST(test_expression_arithmetic_keeps_int_or_promotes_to_double) {
  domain::Row row;
  auto sum = domain::Evaluate(
      *Binary(domain::BinaryOp::kAdd, IntLiteral(1), IntLiteral(2)), row);
  auto mixed = domain::Evaluate(
      *Binary(domain::BinaryOp::kAdd, IntLiteral(1), DoubleLiteral(2.5)), row);
  auto difference = domain::Evaluate(
      *Binary(domain::BinaryOp::kSub, IntLiteral(5), IntLiteral(7)), row);
  CHECK(IsInt(sum, 3));
  CHECK(IsDouble(mixed, 3.5));
  CHECK(IsInt(difference, -2));
}

TEST(test_expression_and_or_truth_table) {
  const bool truth_values[] = {true, false};
  for (bool lhs_is_true : truth_values) {
    for (bool rhs_is_true : truth_values) {
      auto and_expr = Binary(domain::BinaryOp::kAnd, Boolean(lhs_is_true),
                             Boolean(rhs_is_true));
      auto or_expr = Binary(domain::BinaryOp::kOr, Boolean(lhs_is_true),
                            Boolean(rhs_is_true));
      CHECK_EQ(IsTrue(*and_expr), lhs_is_true && rhs_is_true);
      CHECK_EQ(IsFalse(*and_expr), !(lhs_is_true && rhs_is_true));
      CHECK_EQ(IsTrue(*or_expr), lhs_is_true || rhs_is_true);
      CHECK_EQ(IsFalse(*or_expr), !(lhs_is_true || rhs_is_true));
    }
  }
}

TEST(test_aggregate_sum_keeps_argument_type) {
  domain::AggregateAccumulator int_sum(domain::AggregateFn::kSum,
                                       domain::DataType::kInt);
  for (int64_t value : {1, 2, 3}) {
    CHECK(int_sum.Add(domain::Value(value)));
  }
  domain::AggregateAccumulator double_sum(domain::AggregateFn::kSum,
                                          domain::DataType::kDouble);
  for (double value : {1.5, 2.5}) {
    CHECK(double_sum.Add(domain::Value(value)));
  }
  CHECK(IsInt(int_sum.Result(), 6));
  CHECK(IsDouble(double_sum.Result(), 4.0));
}

TEST(test_aggregate_empty_and_single_input) {
  domain::AggregateAccumulator empty_count(domain::AggregateFn::kCount,
                                           domain::DataType::kInt);
  domain::AggregateAccumulator empty_int_sum(domain::AggregateFn::kSum,
                                             domain::DataType::kInt);
  domain::AggregateAccumulator empty_double_sum(domain::AggregateFn::kSum,
                                                domain::DataType::kDouble);
  CHECK(IsInt(empty_count.Result(), 0));
  CHECK(IsInt(empty_int_sum.Result(), 0));
  CHECK(IsDouble(empty_double_sum.Result(), 0.0));

  domain::AggregateAccumulator one_count(domain::AggregateFn::kCount,
                                         domain::DataType::kInt);
  domain::AggregateAccumulator one_int_sum(domain::AggregateFn::kSum,
                                           domain::DataType::kInt);
  domain::AggregateAccumulator one_double_sum(domain::AggregateFn::kSum,
                                              domain::DataType::kDouble);
  CHECK(one_count.Add(domain::Value(int64_t{7})));
  CHECK(one_int_sum.Add(domain::Value(int64_t{7})));
  CHECK(one_double_sum.Add(domain::Value(7.25)));
  CHECK(IsInt(one_count.Result(), 1));
  CHECK(IsInt(one_int_sum.Result(), 7));
  CHECK(IsDouble(one_double_sum.Result(), 7.25));
}

TEST(test_expression_integer_overflow_returns_nullopt) {
  domain::Row row;
  auto above_max = domain::Evaluate(
      *Binary(domain::BinaryOp::kAdd, IntLiteral(kInt64Max), IntLiteral(1)),
      row);
  auto below_min = domain::Evaluate(
      *Binary(domain::BinaryOp::kSub, IntLiteral(kInt64Min), IntLiteral(1)),
      row);
  CHECK(!above_max.has_value());
  CHECK(!below_min.has_value());

  domain::AggregateAccumulator sum(domain::AggregateFn::kSum,
                                   domain::DataType::kInt);
  CHECK(sum.Add(domain::Value(kInt64Max)));
  CHECK(!sum.Add(domain::Value(int64_t{1})));
}

TEST(test_expression_comparison_boundaries) {
  CHECK(IsTrue(*Binary(domain::BinaryOp::kLe, IntLiteral(1), IntLiteral(1))));
  CHECK(IsTrue(*Binary(domain::BinaryOp::kGe, IntLiteral(1), IntLiteral(1))));
  CHECK(IsFalse(*Binary(domain::BinaryOp::kNe, IntLiteral(1), IntLiteral(1))));
}

TEST(test_value_format_double_is_fixed_with_decimal_point) {
  CHECK_EQ(domain::FormatValue(domain::Value(2.0)), std::string("2.0"));
  CHECK_EQ(domain::FormatValue(domain::Value(100000.0)),
           std::string("100000.0"));
  CHECK_EQ(domain::FormatValue(domain::Value(0.00001)), std::string("0.00001"));
  CHECK_EQ(domain::FormatValue(domain::Value(1e20)),
           std::string("100000000000000000000.0"));
  CHECK_EQ(domain::FormatValue(domain::Value(-0.5)), std::string("-0.5"));
}

TEST(test_expression_non_finite_double_returns_nullopt) {
  domain::Row row;
  auto sum =
      domain::Evaluate(*Binary(domain::BinaryOp::kAdd, DoubleLiteral(1e308),
                               DoubleLiteral(1e308)),
                       row);
  CHECK(!sum.has_value());

  domain::AggregateAccumulator accumulator(domain::AggregateFn::kSum,
                                           domain::DataType::kDouble);
  CHECK(accumulator.Add(domain::Value(1e308)));
  CHECK(!accumulator.Add(domain::Value(1e308)));
}
