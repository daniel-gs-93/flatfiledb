#include "strata/domain/scalar_functions.h"

#include <cassert>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace strata::domain {

namespace {

constexpr const char* kOverflowMessage = "numeric overflow";
constexpr double kTwoPow52 = 4503599627370496.0;
constexpr int kMaxRoundDigits = 15;
// Exact in a double: 10^15 < 2^53.
constexpr double kPowersOfTen[kMaxRoundDigits + 1] = {
    1e0, 1e1, 1e2,  1e3,  1e4,  1e5,  1e6,  1e7,
    1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15};

std::nullopt_t Fail(std::string* failure, std::string message) {
  if (failure != nullptr) {
    *failure = std::move(message);
  }
  return std::nullopt;
}

char FoldAscii(char letter) {
  if (letter >= 'A' && letter <= 'Z') {
    return static_cast<char>(letter - 'A' + 'a');
  }
  return letter;
}

bool IsContinuationByte(char letter) {
  const unsigned char code = static_cast<unsigned char>(letter);
  return code >= 0x80 && code <= 0xBF;
}

// Bytes of the UTF-8 character that starts at text[position]: the length its
// lead byte announces when that many bytes remain and the followers are
// continuation bytes, otherwise 1 (a stray, truncated or invalid byte).
// Precondition: position < text.size().
std::size_t CharLength(std::string_view text, std::size_t position) {
  const unsigned char lead = static_cast<unsigned char>(text[position]);
  std::size_t length = 1;
  if (lead >= 0xC2 && lead <= 0xDF) {
    length = 2;
  } else if (lead >= 0xE0 && lead <= 0xEF) {
    length = 3;
  } else if (lead >= 0xF0 && lead <= 0xF4) {
    length = 4;
  }
  if (length == 1 || position + length > text.size()) {
    return 1;
  }
  for (std::size_t i = 1; i < length; ++i) {
    if (!IsContinuationByte(text[position + i])) {
      return 1;
    }
  }
  return length;
}

std::size_t CountChars(std::string_view text) {
  std::size_t count = 0;
  for (std::size_t position = 0; position < text.size();
       position += CharLength(text, position)) {
    ++count;
  }
  return count;
}

std::string MapAscii(const std::string& text, bool is_upper) {
  std::string mapped = text;
  for (char& letter : mapped) {
    if (is_upper && letter >= 'a' && letter <= 'z') {
      letter = static_cast<char>(letter - 'a' + 'A');
    } else if (!is_upper && letter >= 'A' && letter <= 'Z') {
      letter = static_cast<char>(letter - 'A' + 'a');
    }
  }
  return mapped;
}

bool IsBlankByte(char letter) { return letter == ' ' || letter == '\t'; }

std::string Trim(const std::string& text) {
  std::size_t begin = 0;
  while (begin < text.size() && IsBlankByte(text[begin])) {
    ++begin;
  }
  std::size_t end = text.size();
  while (end > begin && IsBlankByte(text[end - 1])) {
    --end;
  }
  return text.substr(begin, end - begin);
}

// Characters at 1-based positions [begin, end), already clipped to
// [1, character count + 1].
std::string CharRange(const std::string& text, int64_t begin, int64_t end) {
  std::size_t byte_begin = 0;
  std::size_t byte_end = text.size();
  int64_t position = 1;
  std::size_t offset = 0;
  while (offset < text.size() && position < end) {
    if (position == begin) {
      byte_begin = offset;
    }
    offset += CharLength(text, offset);
    ++position;
    if (position == end) {
      byte_end = offset;
    }
  }
  if (begin >= position) {
    return std::string();
  }
  return text.substr(byte_begin, byte_end - byte_begin);
}

// length is absent for the two-argument form.
std::optional<Value> Substr(const std::string& text, int64_t start,
                            const int64_t* length, std::string* failure) {
  if (length != nullptr && *length < 0) {
    return Fail(failure, "negative substring length");
  }
  const int64_t character_count = static_cast<int64_t>(CountChars(text));
  int64_t end = character_count + 1;
  if (length != nullptr) {
    int64_t requested_end = 0;
    // length is not negative, so an overflow can only be upward.
    if (__builtin_add_overflow(start, *length, &requested_end)) {
      requested_end = std::numeric_limits<int64_t>::max();
    }
    if (requested_end < end) {
      end = requested_end;
    }
  }
  const int64_t begin = start < 1 ? 1 : start;
  if (begin >= end) {
    return Value(std::string());
  }
  return Value(CharRange(text, begin, end));
}

// Rounds magnitude * scale to an integer by the binary value of magnitude:
// above half rounds up, below half down, an exact half up. std::fma returns the
// rounding error of magnitude * scale, which tells an exact half from a product
// that only rounded up to one. Needs magnitude * scale below 2^52.
double RoundScaledHalfUp(double magnitude, double scale) {
  const double scaled = magnitude * scale;
  const double residual = std::fma(magnitude, scale, -scaled);
  const double lower = std::floor(scaled);
  const double fraction = scaled - lower;
  const bool is_round_up =
      fraction > 0.5 || (fraction == 0.5 && residual >= 0.0);
  return is_round_up ? lower + 1.0 : lower;
}

// Rounds to `digits` decimal places by the binary value of value: above half
// rounds away from zero, below half toward zero, an exact half away from zero.
double RoundHalfAwayFromZero(double value, int digits) {
  const double magnitude = std::fabs(value);
  if (magnitude >= kTwoPow52) {
    return value;
  }  // already an integer
  const double scale = kPowersOfTen[digits];
  if (magnitude * scale >= kTwoPow52) {
    // The product is too large for the fma test, but a double of this size can
    // still have a fractional part. The integer part is exact and the
    // fractional part times scale is below 2^52, so round the fraction alone.
    // The final addition rounds a second time; it differs from the nearest
    // DOUBLE only when the decimal result lies within 2^-54 of a midpoint
    // between two DOUBLEs.
    const double integer_part = std::floor(magnitude);
    const double rounded_fraction =
        RoundScaledHalfUp(magnitude - integer_part, scale);
    return std::copysign(integer_part + rounded_fraction / scale, value);
  }
  return std::copysign(RoundScaledHalfUp(magnitude, scale) / scale, value);
}

std::optional<Value> Abs(const Value& argument, std::string* failure) {
  if (const auto* integer = std::get_if<int64_t>(&argument)) {
    if (*integer == std::numeric_limits<int64_t>::min()) {
      return Fail(failure, kOverflowMessage);
    }
    return Value(*integer < 0 ? -*integer : *integer);
  }
  return Value(std::fabs(std::get<double>(argument)));
}

std::optional<Value> Round(const std::vector<Value>& arguments,
                           std::string* failure) {
  int64_t digits = 0;
  if (arguments.size() == 2) {
    digits = std::get<int64_t>(arguments[1]);
  }
  if (digits < 0 || digits > kMaxRoundDigits) {
    return Fail(failure, "ROUND digits out of range");
  }
  return Value(
      RoundHalfAwayFromZero(ToDouble(arguments[0]), static_cast<int>(digits)));
}

bool IsDigit(char letter) { return letter >= '0' && letter <= '9'; }

std::string CastFailure(const std::string& text, const char* type_name) {
  return "cannot cast '" + text + "' to " + type_name;
}

// [+-]?[0-9]+ within the int64_t range.
std::optional<int64_t> ParseIntegerImpl(const std::string& text) {
  std::size_t position = 0;
  bool is_negative = false;
  if (position < text.size() &&
      (text[position] == '+' || text[position] == '-')) {
    is_negative = text[position] == '-';
    ++position;
  }
  if (position == text.size()) {
    return std::nullopt;
  }
  const uint64_t limit =
      static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) +
      (is_negative ? 1 : 0);
  uint64_t magnitude = 0;
  for (; position < text.size(); ++position) {
    if (!IsDigit(text[position])) {
      return std::nullopt;
    }
    const uint64_t digit = static_cast<uint64_t>(text[position] - '0');
    if (magnitude > (limit - digit) / 10) {
      return std::nullopt;
    }
    magnitude = magnitude * 10 + digit;
  }
  if (!is_negative) {
    return static_cast<int64_t>(magnitude);
  }
  if (magnitude == limit) {
    return std::numeric_limits<int64_t>::min();
  }
  return -static_cast<int64_t>(magnitude);
}

// [+-]?[0-9]+(\.[0-9]+)? with a finite result.
std::optional<double> ParseDecimalImpl(const std::string& text) {
  std::size_t position = 0;
  if (position < text.size() &&
      (text[position] == '+' || text[position] == '-')) {
    ++position;
  }
  std::size_t digits = 0;
  while (position < text.size() && IsDigit(text[position])) {
    ++position;
    ++digits;
  }
  if (digits == 0) {
    return std::nullopt;
  }
  if (position < text.size() && text[position] == '.') {
    ++position;
    std::size_t fraction_digits = 0;
    while (position < text.size() && IsDigit(text[position])) {
      ++position;
      ++fraction_digits;
    }
    if (fraction_digits == 0) {
      return std::nullopt;
    }
  }
  if (position != text.size()) {
    return std::nullopt;
  }
  // std::from_chars ignores the locale, reads a subnormal value, and reports
  // a value beyond the largest double or below the smallest subnormal as
  // result_out_of_range. It does not accept a '+' sign.
  const char* begin = text.data() + (text.front() == '+' ? 1 : 0);
  const char* end = text.data() + text.size();
  double value = 0.0;
  const std::from_chars_result result = std::from_chars(begin, end, value);
  if (result.ec != std::errc() || result.ptr != end || !std::isfinite(value)) {
    return std::nullopt;
  }
  return value;
}

std::optional<Value> CastToInt(const Value& argument, std::string* failure) {
  if (const auto* integer = std::get_if<int64_t>(&argument)) {
    return Value(*integer);
  }
  if (const auto* number = std::get_if<double>(&argument)) {
    // Both bounds are exactly representable as doubles.
    if (!(*number >= -9223372036854775808.0 &&
          *number < 9223372036854775808.0)) {
      return Fail(failure, kOverflowMessage);
    }
    return Value(static_cast<int64_t>(*number));
  }
  const std::string& text = std::get<std::string>(argument);
  std::optional<int64_t> parsed = ParseIntegerImpl(text);
  if (!parsed) {
    return Fail(failure, CastFailure(text, "INT"));
  }
  return Value(*parsed);
}

std::optional<Value> CastToDouble(const Value& argument, std::string* failure) {
  if (const auto* text = std::get_if<std::string>(&argument)) {
    std::optional<double> parsed = ParseDecimalImpl(*text);
    if (!parsed) {
      return Fail(failure, CastFailure(*text, "DOUBLE"));
    }
    return Value(*parsed);
  }
  return Value(ToDouble(argument));
}

std::optional<Value> Cast(const Value& argument, ExprType result_type,
                          std::string* failure) {
  switch (result_type) {
    case ExprType::kInt:
      return CastToInt(argument, failure);
    case ExprType::kDouble:
      return CastToDouble(argument, failure);
    default:
      break;
  }
  if (std::holds_alternative<std::string>(argument)) {
    return argument;
  }
  return Value(FormatValue(argument));
}

}  // namespace

std::optional<int64_t> ParseIntegerText(const std::string& text) {
  return ParseIntegerImpl(text);
}

std::optional<double> ParseDecimalText(const std::string& text) {
  return ParseDecimalImpl(text);
}

bool LikeMatches(const std::string& text, const std::string& pattern) {
  constexpr std::size_t kNoStar = std::numeric_limits<std::size_t>::max();
  std::size_t text_position = 0;
  std::size_t pattern_position = 0;
  std::size_t star_pattern = kNoStar;  // pattern position after the last '%'
  std::size_t star_text = 0;           // text position that '%' has reached
  while (text_position < text.size()) {
    if (pattern_position < pattern.size()) {
      const char pattern_byte = pattern[pattern_position];
      if (pattern_byte == '%') {
        ++pattern_position;
        star_pattern = pattern_position;
        star_text = text_position;
        continue;
      }
      if (pattern_byte == '_') {
        text_position += CharLength(text, text_position);
        ++pattern_position;
        continue;
      }
      if (FoldAscii(pattern_byte) == FoldAscii(text[text_position])) {
        ++text_position;
        ++pattern_position;
        continue;
      }
    }
    // Mismatch: let the last '%' absorb one more character and retry.
    if (star_pattern == kNoStar) {
      return false;
    }
    star_text += CharLength(text, star_text);
    text_position = star_text;
    pattern_position = star_pattern;
  }
  while (pattern_position < pattern.size() &&
         pattern[pattern_position] == '%') {
    ++pattern_position;
  }
  return pattern_position == pattern.size();
}

std::optional<Value> ApplyScalarFunction(ScalarFn fn, ExprType result_type,
                                         const std::vector<Value>& arguments,
                                         std::string* failure) {
  assert(!arguments.empty());
  switch (fn) {
    case ScalarFn::kLower:
      return Value(MapAscii(std::get<std::string>(arguments[0]), false));
    case ScalarFn::kUpper:
      return Value(MapAscii(std::get<std::string>(arguments[0]), true));
    case ScalarFn::kLength:
      return Value(static_cast<int64_t>(
          CountChars(std::get<std::string>(arguments[0]))));
    case ScalarFn::kTrim:
      return Value(Trim(std::get<std::string>(arguments[0])));
    case ScalarFn::kSubstr: {
      const int64_t start = std::get<int64_t>(arguments[1]);
      if (arguments.size() == 2) {
        return Substr(std::get<std::string>(arguments[0]), start, nullptr,
                      failure);
      }
      const int64_t length = std::get<int64_t>(arguments[2]);
      return Substr(std::get<std::string>(arguments[0]), start, &length,
                    failure);
    }
    case ScalarFn::kAbs:
      return Abs(arguments[0], failure);
    case ScalarFn::kRound:
      return Round(arguments, failure);
    case ScalarFn::kCast:
      return Cast(arguments[0], result_type, failure);
  }
  assert(false && "unknown scalar function");
  return std::nullopt;
}

}  // namespace strata::domain
