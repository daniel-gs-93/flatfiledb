#include "flatfiledb/domain/value.h"

#include <cassert>
#include <charconv>
#include <string>
#include <string_view>
#include <system_error>

namespace flatfiledb::domain {

namespace {

char ToLowerChar(char letter) {
  if (letter >= 'A' && letter <= 'Z') {
    return static_cast<char>(letter - 'A' + 'a');
  }
  return letter;
}

int SignOf(int value) {
  if (value < 0) {
    return -1;
  }
  if (value > 0) {
    return 1;
  }
  return 0;
}

}  // namespace

std::string ToLower(std::string_view text) {
  std::string lowered;
  lowered.reserve(text.size());
  for (char letter : text) {
    lowered.push_back(ToLowerChar(letter));
  }
  return lowered;
}

DataType TypeOf(const Value& value) {
  if (std::holds_alternative<int64_t>(value)) {
    return DataType::kInt;
  }
  if (std::holds_alternative<double>(value)) {
    return DataType::kDouble;
  }
  return DataType::kText;
}

double ToDouble(const Value& value) {
  if (const auto* integer = std::get_if<int64_t>(&value)) {
    return static_cast<double>(*integer);
  }
  return std::get<double>(value);
}

int CompareValues(const Value& lhs, const Value& rhs) {
  const auto* lhs_text = std::get_if<std::string>(&lhs);
  const auto* rhs_text = std::get_if<std::string>(&rhs);
  if (lhs_text != nullptr || rhs_text != nullptr) {
    assert(lhs_text != nullptr && rhs_text != nullptr);
    return SignOf(lhs_text->compare(*rhs_text));
  }
  const auto* lhs_int = std::get_if<int64_t>(&lhs);
  const auto* rhs_int = std::get_if<int64_t>(&rhs);
  if (lhs_int != nullptr && rhs_int != nullptr) {
    if (*lhs_int < *rhs_int) {
      return -1;
    }
    if (*lhs_int > *rhs_int) {
      return 1;
    }
    return 0;
  }
  double lhs_double = ToDouble(lhs);
  double rhs_double = ToDouble(rhs);
  if (lhs_double < rhs_double) {
    return -1;
  }
  if (lhs_double > rhs_double) {
    return 1;
  }
  return 0;
}

bool ValuesEqual(const Value& lhs, const Value& rhs) {
  return CompareValues(lhs, rhs) == 0;
}

std::string FormatValue(const Value& value) {
  if (const auto* integer = std::get_if<int64_t>(&value)) {
    return std::to_string(*integer);
  }
  if (const auto* text = std::get_if<std::string>(&value)) {
    return *text;
  }
  // The longest finite double in fixed notation has about 330 characters.
  char buffer[512];
  auto result =
      std::to_chars(buffer, buffer + sizeof(buffer), std::get<double>(value),
                    std::chars_format::fixed);
  assert(result.ec == std::errc());
  std::string formatted(buffer, result.ptr);
  if (formatted.find('.') == std::string::npos) {
    formatted += ".0";
  }
  return formatted;
}

std::optional<Value> CoerceValue(const Value& value, DataType target) {
  DataType source = TypeOf(value);
  if (source == target) {
    return value;
  }
  if (source == DataType::kInt && target == DataType::kDouble) {
    return Value(static_cast<double>(std::get<int64_t>(value)));
  }
  return std::nullopt;
}

const char* ErrorKindName(ErrorKind kind) {
  switch (kind) {
    case ErrorKind::kBind:
      return "bind";
    case ErrorKind::kUnsupported:
      return "unsupported";
    case ErrorKind::kConstraintViolation:
      return "constraint violation";
    case ErrorKind::kRuntime:
      return "runtime";
    case ErrorKind::kIo:
      return "io";
  }
  return "";
}

}  // namespace flatfiledb::domain
