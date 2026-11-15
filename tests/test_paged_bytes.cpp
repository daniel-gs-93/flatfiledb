#include <bit>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "strata/domain/value.h"
#include "strata/paged/byte_codec.h"
#include "strata/paged/crc32c.h"
#include "strata/paged/tuple_codec.h"
#include "test_harness.h"
#include "test_paged_support.h"

// CRC-32C, little-endian integers and the tuple codec.

namespace {

using strata::domain::DataType;
using strata::domain::Row;
using strata::domain::TableSchema;
using strata::domain::Value;
using strata::paged::ByteView;
using strata::paged::Crc32c;
using strata::paged::ReadLe16;
using strata::paged::ReadLe32;
using strata::paged::ReadLe64;
using strata::paged::TupleCodec;
using strata::paged::WriteLe16;
using strata::paged::WriteLe32;
using strata::paged::WriteLe64;
using strata::testing::BytesFrom;
using strata::testing::BytesOf;
using strata::testing::Column;
using strata::testing::MakeSchema;
using strata::testing::ViewOf;

std::uint32_t CrcOf(const std::vector<std::uint8_t>& bytes) {
  return Crc32c(bytes.data(), bytes.size());
}

// The CRC-32C definition evaluated bit by bit, independent of the table.
std::uint32_t BitwiseCrc32c(const std::vector<std::uint8_t>& bytes) {
  std::uint32_t crc = 0xFFFFFFFFu;
  for (std::uint8_t byte_value : bytes) {
    crc ^= byte_value;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1u) != 0 ? (crc >> 1) ^ 0x82F63B78u : crc >> 1;
    }
  }
  return crc ^ 0xFFFFFFFFu;
}

std::uint64_t BitsOf(double number) {
  return std::bit_cast<std::uint64_t>(number);
}

double DoubleFromBits(std::uint64_t bits) {
  return std::bit_cast<double>(bits);
}

// Row equality where DOUBLE compares by bit pattern, so -0.0 and NaN count.
bool RowsBitwiseEqual(const Row& left, const Row& right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t i = 0; i < left.size(); ++i) {
    if (left[i].index() != right[i].index()) {
      return false;
    }
    if (std::holds_alternative<double>(left[i])) {
      if (BitsOf(std::get<double>(left[i])) !=
          BitsOf(std::get<double>(right[i]))) {
        return false;
      }
    } else if (!(left[i] == right[i])) {
      return false;
    }
  }
  return true;
}

TableSchema SingleColumn(DataType type) {
  return MakeSchema("t", {Column("v", type)});
}

}  // namespace

TEST(test_paged_crc32c_matches_known_vectors) {
  CHECK_EQ(CrcOf({}), 0x00000000u);
  CHECK_EQ(CrcOf(BytesOf("123456789")), 0xE3069283u);
  CHECK_EQ(CrcOf(std::vector<std::uint8_t>(32, 0x00)), 0x8A9136AAu);
  CHECK_EQ(CrcOf(std::vector<std::uint8_t>(32, 0xFF)), 0x62A8AB43u);
  std::vector<std::uint8_t> ascending;
  std::vector<std::uint8_t> descending;
  for (int i = 0; i < 32; ++i) {
    ascending.push_back(static_cast<std::uint8_t>(i));
    descending.push_back(static_cast<std::uint8_t>(31 - i));
  }
  CHECK_EQ(CrcOf(ascending), 0x46DD794Eu);
  CHECK_EQ(CrcOf(descending), 0x113FDB5Cu);
  CHECK_EQ(CrcOf(BytesFrom({0x01})), 0xA016D052u);
}

TEST(test_paged_crc32c_matches_bitwise_reference) {
  std::mt19937 generator(20261111);
  std::uniform_int_distribution<int> byte_distribution(0, 255);
  for (int length = 0; length <= 300; ++length) {
    for (int repeat = 0; repeat < 3; ++repeat) {
      std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
      for (std::uint8_t& byte_value : bytes) {
        byte_value = static_cast<std::uint8_t>(byte_distribution(generator));
      }
      CHECK_EQ(CrcOf(bytes), BitwiseCrc32c(bytes));
    }
  }
}

TEST(test_paged_byte_codec_is_little_endian_and_round_trips) {
  const std::uint16_t values16[] = {0, 1, 0xFFFF, 0x0102};
  for (std::uint16_t value : values16) {
    std::uint8_t bytes[2];
    WriteLe16(bytes, value);
    CHECK_EQ(static_cast<int>(bytes[0]), static_cast<int>(value & 0xFF));
    CHECK_EQ(static_cast<int>(bytes[1]), static_cast<int>(value >> 8));
    CHECK_EQ(ReadLe16(bytes), value);
  }
  const std::uint32_t values32[] = {0, 1, 0xFFFFFFFFu, 0x01020304u};
  for (std::uint32_t value : values32) {
    std::uint8_t bytes[4];
    WriteLe32(bytes, value);
    for (int i = 0; i < 4; ++i) {
      CHECK_EQ(static_cast<int>(bytes[i]),
               static_cast<int>((value >> (8 * i)) & 0xFF));
    }
    CHECK_EQ(ReadLe32(bytes), value);
  }
  const std::uint64_t values64[] = {0, 1, 0xFFFFFFFFFFFFFFFFull,
                                    0x0102030405060708ull};
  for (std::uint64_t value : values64) {
    std::uint8_t bytes[8];
    WriteLe64(bytes, value);
    for (int i = 0; i < 8; ++i) {
      CHECK_EQ(static_cast<int>(bytes[i]),
               static_cast<int>((value >> (8 * i)) & 0xFF));
    }
    CHECK_EQ(ReadLe64(bytes), value);
  }
  std::uint8_t pattern[8];
  WriteLe64(pattern, 0x0102030405060708ull);
  CHECK_EQ(static_cast<int>(pattern[0]), 0x08);
  CHECK_EQ(static_cast<int>(pattern[7]), 0x01);
}

TEST(test_paged_tuple_round_trips_every_value_class) {
  const std::vector<std::int64_t> ints = {
      0, 1, -1, std::numeric_limits<std::int64_t>::min(),
      std::numeric_limits<std::int64_t>::max()};
  for (std::int64_t number : ints) {
    const TableSchema schema = SingleColumn(DataType::kInt);
    const Row row = {Value(number)};
    auto encoded = TupleCodec::Encode(schema, row);
    CHECK_OK(encoded);
    if (!encoded.ok()) {
      continue;
    }
    CHECK_EQ(encoded.value().size(), static_cast<std::size_t>(8));
    for (int i = 0; i < 8; ++i) {
      CHECK_EQ(static_cast<int>(encoded.value()[static_cast<std::size_t>(i)]),
               static_cast<int>(
                   (static_cast<std::uint64_t>(number) >> (8 * i)) & 0xFF));
    }
    auto decoded = TupleCodec::Decode(schema, ViewOf(encoded.value()));
    CHECK_OK(decoded);
    if (decoded.ok()) {
      CHECK(RowsBitwiseEqual(decoded.value(), row));
    }
  }

  const std::vector<double> doubles = {
      0.0,
      -0.0,
      0.1,
      DBL_MIN,
      std::numeric_limits<double>::denorm_min(),
      DBL_MAX,
      std::numeric_limits<double>::infinity(),
      -std::numeric_limits<double>::infinity(),
      DoubleFromBits(0x7FF8000000000001ull)};
  for (double number : doubles) {
    const TableSchema schema = SingleColumn(DataType::kDouble);
    const Row row = {Value(number)};
    auto encoded = TupleCodec::Encode(schema, row);
    CHECK_OK(encoded);
    if (!encoded.ok()) {
      continue;
    }
    CHECK_EQ(encoded.value().size(), static_cast<std::size_t>(8));
    for (int i = 0; i < 8; ++i) {
      CHECK_EQ(static_cast<int>(encoded.value()[static_cast<std::size_t>(i)]),
               static_cast<int>((BitsOf(number) >> (8 * i)) & 0xFF));
    }
    auto decoded = TupleCodec::Decode(schema, ViewOf(encoded.value()));
    CHECK_OK(decoded);
    if (decoded.ok()) {
      CHECK(RowsBitwiseEqual(decoded.value(), row));
    }
  }

  const std::vector<std::string> texts = {"",
                                          "a",
                                          "x,y\"z\nw",
                                          std::string("nul\0byte", 8),
                                          "\xE4\xBD\xA0\xE5\xA5\xBD",
                                          std::string(4000, 'q')};
  for (const std::string& text : texts) {
    const TableSchema schema = SingleColumn(DataType::kText);
    const Row row = {Value(text)};
    auto encoded = TupleCodec::Encode(schema, row);
    CHECK_OK(encoded);
    if (!encoded.ok()) {
      continue;
    }
    CHECK_EQ(encoded.value().size(), text.size() + 2);
    CHECK_EQ(static_cast<int>(encoded.value()[0]),
             static_cast<int>(text.size() & 0xFF));
    CHECK_EQ(static_cast<int>(encoded.value()[1]),
             static_cast<int>(text.size() >> 8));
    auto decoded = TupleCodec::Decode(schema, ViewOf(encoded.value()));
    CHECK_OK(decoded);
    if (decoded.ok()) {
      CHECK(RowsBitwiseEqual(decoded.value(), row));
    }
  }

  // Columns are written in schema order with no separator: INT 1, TEXT "ab",
  // DOUBLE -0.0.
  const TableSchema mixed = MakeSchema(
      "mixed", {Column("a", DataType::kInt), Column("b", DataType::kText),
                Column("c", DataType::kDouble)});
  auto encoded = TupleCodec::Encode(
      mixed, {Value(std::int64_t{1}), Value(std::string("ab")), Value(-0.0)});
  CHECK_OK(encoded);
  if (encoded.ok()) {
    const std::vector<std::uint8_t> expected =
        BytesFrom({0x01, 0,   0, 0, 0, 0, 0, 0, 0x02, 0x00,
                   'a',  'b', 0, 0, 0, 0, 0, 0, 0,    0x80});
    CHECK(encoded.value() == expected);
  }
}

TEST(test_paged_tuple_encode_rejects_invalid_rows) {
  const TableSchema two_columns = MakeSchema(
      "t", {Column("id", DataType::kInt), Column("name", DataType::kText)});
  CHECK_ERROR(TupleCodec::Encode(two_columns, {Value(std::int64_t{1})}),
              kInvalidArgument);
  CHECK_ERROR(TupleCodec::Encode(
                  two_columns, {Value(std::int64_t{1}), Value(std::string("a")),
                                Value(std::int64_t{2})}),
              kInvalidArgument);
  CHECK_ERROR(TupleCodec::Encode(two_columns, {Value(std::string("x")),
                                               Value(std::string("a"))}),
              kInvalidArgument);
  CHECK_ERROR(TupleCodec::Encode(two_columns, {Value(std::int64_t{1}),
                                               Value(std::int64_t{2})}),
              kInvalidArgument);
  const TableSchema number = SingleColumn(DataType::kDouble);
  CHECK_ERROR(TupleCodec::Encode(number, {Value(std::int64_t{1})}),
              kInvalidArgument);

  const TableSchema text = SingleColumn(DataType::kText);
  CHECK_OK(TupleCodec::Encode(text, {Value(std::string(4058, 'x'))}));
  CHECK_ERROR(TupleCodec::Encode(text, {Value(std::string(4059, 'x'))}),
              kTooLarge);
  CHECK_ERROR(TupleCodec::Encode(text, {Value(std::string(65536, 'x'))}),
              kTooLarge);
}

TEST(test_paged_tuple_decode_rejects_malformed_bytes) {
  const TableSchema schema = MakeSchema(
      "t", {Column("id", DataType::kInt), Column("name", DataType::kText)});
  auto valid = TupleCodec::Encode(
      schema, {Value(std::int64_t{5}), Value(std::string("hello"))});
  CHECK_OK(valid);
  if (!valid.ok()) {
    return;
  }
  CHECK_OK(TupleCodec::Decode(schema, ViewOf(valid.value())));

  std::vector<std::uint8_t> shorter = valid.value();
  shorter.pop_back();
  CHECK_ERROR(TupleCodec::Decode(schema, ViewOf(shorter)), kCorrupt);

  std::vector<std::uint8_t> bad_length = valid.value();
  bad_length[8] = 0xFF;  // the TEXT length now exceeds the remaining bytes
  CHECK_ERROR(TupleCodec::Decode(schema, ViewOf(bad_length)), kCorrupt);

  std::vector<std::uint8_t> longer = valid.value();
  longer.push_back(0x00);
  CHECK_ERROR(TupleCodec::Decode(schema, ViewOf(longer)), kCorrupt);

  const std::vector<std::uint8_t> empty;
  CHECK_ERROR(TupleCodec::Decode(schema, ViewOf(empty)), kCorrupt);
}
