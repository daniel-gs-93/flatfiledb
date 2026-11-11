#include "flatfiledb/paged/tuple_codec.h"

#include <bit>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "flatfiledb/paged/byte_codec.h"

namespace flatfiledb::paged {

namespace {

constexpr std::size_t kNumberSize = 8;
constexpr std::size_t kTextLengthSize = 2;
constexpr std::size_t kMaxTextLength = 65535;

const char* TypeName(domain::DataType type) {
  switch (type) {
    case domain::DataType::kInt:
      return "INT";
    case domain::DataType::kDouble:
      return "DOUBLE";
    case domain::DataType::kText:
      return "TEXT";
  }
  return "?";
}

}  // namespace

PageResult<std::vector<std::uint8_t>> TupleCodec::Encode(
    const domain::TableSchema& schema, const domain::Row& row) {
  if (row.size() != schema.columns.size()) {
    return MakeError(PageErrorCode::kInvalidArgument,
                     "row has " + std::to_string(row.size()) +
                         " values but table " + schema.name + " has " +
                         std::to_string(schema.columns.size()) + " columns");
  }
  // One pass to validate types and compute the size, so nothing is written
  // for a row that is rejected.
  std::size_t total_size = 0;
  for (std::size_t column = 0; column < row.size(); ++column) {
    const domain::ColumnDef& definition = schema.columns[column];
    if (domain::TypeOf(row[column]) != definition.type) {
      return MakeError(PageErrorCode::kInvalidArgument,
                       "column " + definition.name + " is " +
                           TypeName(definition.type) +
                           " but the value has another type");
    }
    if (definition.type == domain::DataType::kText) {
      const std::size_t text_length = std::get<std::string>(row[column]).size();
      if (text_length > kMaxTextLength) {
        return MakeError(PageErrorCode::kTooLarge,
                         "TEXT value of column " + definition.name + " has " +
                             std::to_string(text_length) +
                             " bytes; the limit is 65535");
      }
      total_size += kTextLengthSize + text_length;
    } else {
      total_size += kNumberSize;
    }
  }
  if (total_size > kMaxTupleSize) {
    return MakeError(PageErrorCode::kTooLarge,
                     "tuple has " + std::to_string(total_size) +
                         " bytes; the limit is " +
                         std::to_string(kMaxTupleSize));
  }

  std::vector<std::uint8_t> bytes(total_size);
  std::size_t position = 0;
  for (std::size_t column = 0; column < row.size(); ++column) {
    const domain::Value& value = row[column];
    switch (schema.columns[column].type) {
      case domain::DataType::kInt:
        WriteLe64(&bytes[position],
                  static_cast<std::uint64_t>(std::get<std::int64_t>(value)));
        position += kNumberSize;
        break;
      case domain::DataType::kDouble: {
        const double number = std::get<double>(value);
        const std::uint64_t bits = std::bit_cast<std::uint64_t>(number);
        WriteLe64(&bytes[position], bits);
        position += kNumberSize;
        break;
      }
      case domain::DataType::kText: {
        const std::string& text = std::get<std::string>(value);
        WriteLe16(&bytes[position], static_cast<std::uint16_t>(text.size()));
        position += kTextLengthSize;
        if (!text.empty()) {
          std::memcpy(&bytes[position], text.data(), text.size());
        }
        position += text.size();
        break;
      }
    }
  }
  return bytes;
}

PageResult<domain::Row> TupleCodec::Decode(const domain::TableSchema& schema,
                                           ByteView bytes) {
  domain::Row row;
  row.reserve(schema.columns.size());
  std::size_t position = 0;
  for (const domain::ColumnDef& definition : schema.columns) {
    const std::size_t remaining = bytes.size - position;
    if (definition.type == domain::DataType::kText) {
      if (remaining < kTextLengthSize) {
        return MakeError(
            PageErrorCode::kCorrupt,
            "tuple ends before the length of column " + definition.name);
      }
      const std::size_t text_length = ReadLe16(bytes.data + position);
      position += kTextLengthSize;
      if (bytes.size - position < text_length) {
        return MakeError(PageErrorCode::kCorrupt,
                         "TEXT length " + std::to_string(text_length) +
                             " of column " + definition.name +
                             " exceeds the remaining " +
                             std::to_string(bytes.size - position) + " bytes");
      }
      row.emplace_back(std::string(
          reinterpret_cast<const char*>(bytes.data + position), text_length));
      position += text_length;
      continue;
    }
    if (remaining < kNumberSize) {
      return MakeError(PageErrorCode::kCorrupt,
                       "tuple ends before column " + definition.name);
    }
    const std::uint64_t bits = ReadLe64(bytes.data + position);
    position += kNumberSize;
    if (definition.type == domain::DataType::kInt) {
      row.emplace_back(static_cast<std::int64_t>(bits));
    } else {
      row.emplace_back(std::bit_cast<double>(bits));
    }
  }
  if (position != bytes.size) {
    return MakeError(PageErrorCode::kCorrupt,
                     std::to_string(bytes.size - position) +
                         " bytes remain after the last column of table " +
                         schema.name);
  }
  return row;
}

}  // namespace flatfiledb::paged
