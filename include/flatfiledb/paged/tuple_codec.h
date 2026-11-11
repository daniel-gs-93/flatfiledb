#ifndef FLATFILEDB_PAGED_TUPLE_CODEC_H_
#define FLATFILEDB_PAGED_TUPLE_CODEC_H_

#include <cstdint>
#include <vector>

#include "flatfiledb/domain/value.h"
#include "flatfiledb/paged/paged_types.h"

namespace flatfiledb::paged {

// Row <-> bytes by the column order of a TableSchema (design document 2.4):
// INT is 8 bytes little-endian, DOUBLE is the 8 bytes of its IEEE 754 bit
// pattern little-endian, TEXT is a 2-byte length plus the bytes. There is no
// column count, type tag or NULL bitmap. Pure.
class TupleCodec {
 public:
  // The encoded bytes are returned by value; the caller owns them.
  // kInvalidArgument: the column count or a value's type differs from the
  // schema (no implicit conversion). kTooLarge: a TEXT value is longer than
  // 65535 bytes, or the tuple is longer than kMaxTupleSize.
  static PageResult<std::vector<std::uint8_t>> Encode(
      const domain::TableSchema& schema, const domain::Row& row);

  // The Row is a copy; it does not refer to bytes. kCorrupt: the bytes end
  // early, a TEXT length exceeds the remaining bytes, or bytes remain after
  // the last column.
  static PageResult<domain::Row> Decode(const domain::TableSchema& schema,
                                        ByteView bytes);
};

}  // namespace flatfiledb::paged

#endif  // FLATFILEDB_PAGED_TUPLE_CODEC_H_
