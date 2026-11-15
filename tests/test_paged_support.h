#ifndef STRATA_TESTS_TEST_PAGED_SUPPORT_H_
#define STRATA_TESTS_TEST_PAGED_SUPPORT_H_

#include <array>
#include <cstdint>
#include <initializer_list>
#include <ostream>
#include <string>
#include <vector>

#include "strata/domain/value.h"
#include "strata/paged/paged_types.h"
#include "strata/paged/slotted_page.h"
#include "test_harness.h"

// Helpers shared by the paged-layer tests (docs/PagedEngineTestDocument.md).

namespace strata::paged {

inline std::ostream& operator<<(std::ostream& stream, const Rid& rid) {
  return stream << "(" << rid.page_id << "," << rid.slot_id << ")";
}

}  // namespace strata::paged

namespace strata::testing {

using Page = std::array<std::uint8_t, paged::kPageSize>;

inline const char* ErrorCodeName(paged::PageErrorCode code) {
  switch (code) {
    case paged::PageErrorCode::kIo:
      return "kIo";
    case paged::PageErrorCode::kCorrupt:
      return "kCorrupt";
    case paged::PageErrorCode::kNoSpace:
      return "kNoSpace";
    case paged::PageErrorCode::kNoFrame:
      return "kNoFrame";
    case paged::PageErrorCode::kNotFound:
      return "kNotFound";
    case paged::PageErrorCode::kTooLarge:
      return "kTooLarge";
    case paged::PageErrorCode::kInvalidArgument:
      return "kInvalidArgument";
    case paged::PageErrorCode::kAlreadyExists:
      return "kAlreadyExists";
    case paged::PageErrorCode::kClosed:
      return "kClosed";
  }
  return "?";
}

// "ok" or the name of the error code.
template <typename T>
std::string ErrorName(const paged::PageResult<T>& result) {
  return result.ok() ? "ok" : ErrorCodeName(result.error().code);
}
inline std::string ErrorName(const paged::Status& status) {
  return status.has_value() ? ErrorCodeName(status->code) : "ok";
}

// "ok" or "<code>: <message>", so a failed CHECK shows why.
template <typename T>
std::string Describe(const paged::PageResult<T>& result) {
  if (result.ok()) {
    return "ok";
  }
  return std::string(ErrorCodeName(result.error().code)) + ": " +
         result.error().message;
}
inline std::string Describe(const paged::Status& status) {
  if (!status.has_value()) {
    return "ok";
  }
  return std::string(ErrorCodeName(status->code)) + ": " + status->message;
}

template <typename T>
std::string MessageOf(const paged::PageResult<T>& result) {
  return result.ok() ? "" : result.error().message;
}
inline std::string MessageOf(const paged::Status& status) {
  return status.has_value() ? status->message : "";
}

}  // namespace strata::testing

#define CHECK_OK(expression) \
  CHECK_EQ(::strata::testing::Describe(expression), std::string("ok"))

#define CHECK_ERROR(expression, code) \
  CHECK_EQ(::strata::testing::ErrorName(expression), std::string(#code))

#define CHECK_MESSAGE_CONTAINS(expression, text)               \
  CHECK(::strata::testing::MessageOf(expression).find(text) != \
        std::string::npos)

namespace strata::testing {

inline paged::ByteView ViewOf(const std::vector<std::uint8_t>& bytes) {
  return paged::ByteView{bytes.data(), bytes.size()};
}

inline paged::ByteView ViewOf(const std::string& text) {
  return paged::ByteView{reinterpret_cast<const std::uint8_t*>(text.data()),
                         text.size()};
}

inline std::vector<std::uint8_t> BytesOf(const std::string& text) {
  return std::vector<std::uint8_t>(text.begin(), text.end());
}

inline std::vector<std::uint8_t> BytesFrom(std::initializer_list<int> values) {
  std::vector<std::uint8_t> bytes;
  for (int value : values) {
    bytes.push_back(static_cast<std::uint8_t>(value));
  }
  return bytes;
}

inline std::string ToString(paged::ByteView view) {
  return std::string(reinterpret_cast<const char*>(view.data), view.size);
}

// A tuple of `size` bytes, all equal to `fill`.
inline std::string Filled(std::size_t size, char fill) {
  return std::string(size, fill);
}

// A formatted heap-data page that has not been sealed.
inline Page FormattedPage(paged::PageId page_id = 7,
                          std::uint32_t table_id = 3) {
  Page page{};
  paged::SlottedPage::Format(page.data(), page_id, table_id);
  return page;
}

inline domain::ColumnDef Column(const std::string& name,
                                domain::DataType type) {
  return domain::ColumnDef{name, type};
}

inline domain::TableSchema MakeSchema(
    const std::string& name, const std::vector<domain::ColumnDef>& columns) {
  domain::TableSchema schema;
  schema.name = name;
  schema.columns = columns;
  schema.primary_key_index = 0;
  return schema;
}

}  // namespace strata::testing

#endif  // STRATA_TESTS_TEST_PAGED_SUPPORT_H_
