#include "flatfiledb/paged/row_keys.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>

namespace flatfiledb::paged {

namespace {

using domain::Value;

void AppendBytes(const void* bytes, std::size_t size, std::string* key) {
  key->append(static_cast<const char*>(bytes), size);
}

void AppendDoubleBytes(double number, std::string* key) {
  // -0.0 and 0.0 compare equal, so they must produce the same bytes.
  if (number == 0.0) {
    number = 0.0;
  }
  const std::uint64_t bits = std::bit_cast<std::uint64_t>(number);
  AppendBytes(&bits, sizeof(bits), key);
}

}  // namespace

void AppendExactKey(const Value& value, std::string* key) {
  if (const auto* integer = std::get_if<std::int64_t>(&value)) {
    key->push_back('I');
    AppendBytes(integer, sizeof(*integer), key);
    return;
  }
  if (const auto* number = std::get_if<double>(&value)) {
    key->push_back('D');
    AppendDoubleBytes(*number, key);
    return;
  }
  // Value has three alternatives and the INT and DOUBLE ones were handled
  // above, so the value holds a string. The length prefix makes the encoding
  // self-delimiting.
  const std::string& text = std::get<std::string>(value);
  const std::uint64_t length = text.size();
  key->push_back('T');
  AppendBytes(&length, sizeof(length), key);
  key->append(text);
}

std::string ExactKey(const Value& value) {
  std::string key;
  AppendExactKey(value, &key);
  return key;
}

std::string JoinKey(const Value& value) {
  if (const auto* text = std::get_if<std::string>(&value)) {
    return "T" + *text;
  }
  std::string key = "N";
  AppendDoubleBytes(domain::ToDouble(value), &key);
  return key;
}

}  // namespace flatfiledb::paged
