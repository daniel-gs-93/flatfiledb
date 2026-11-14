#ifndef FLATFILEDB_PAGED_ROW_STREAM_H_
#define FLATFILEDB_PAGED_ROW_STREAM_H_

#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "flatfiledb/domain/value.h"
#include "flatfiledb/paged/paged_types.h"

// The row layer's view of a table (design document 2.2): a row stream hands
// out the rows of one table one at a time, each with its Rid, and a table
// reader opens streams by table name. The interfaces belong to the paged
// layer; HeapAccess implements them over TableHeap and tests implement them
// over a std::vector. This header includes no storage header, so the operators
// and the mutation planner stay free of pages and pins.

namespace flatfiledb::paged {

// One row of a table and where it is stored. Same content as ScanItem, kept
// here so the row layer does not include table_heap.h.
struct StoredRow {
  Rid rid;
  domain::Row row;
};

// A row, the end of the rows (the optional has no value), or the error that
// ended the stream. After an error every later Next returns the end.
using StreamResult =
    std::variant<std::optional<StoredRow>, domain::ExecutionError>;

// The rows of one table in storage order: page chain order, then slot order
// within a page. Not thread-safe: only the thread that created a stream may
// use it.
class IRowStream {
 public:
  virtual ~IRowStream() = default;

  // The row is returned by value and belongs to the caller.
  virtual StreamResult Next() = 0;

  // The columns of the table, one per value of every row. Returned by value.
  virtual std::vector<domain::ColumnDef> Columns() const = 0;
};

// A stream, or the error that prevented opening it.
using ScanResult =
    std::variant<std::unique_ptr<IRowStream>, domain::ExecutionError>;

// Opens row streams by table name.
class ITableReader {
 public:
  virtual ~ITableReader() = default;

  // The name is matched case-insensitively. A table that does not exist yields
  // ExecutionError{kBind, "unknown table: <table>"}, with the name as given.
  // The stream belongs to the caller and must not outlive the reader.
  // const, and several threads may call it at the same time.
  virtual ScanResult Scan(const std::string& table) const = 0;
};

}  // namespace flatfiledb::paged

#endif  // FLATFILEDB_PAGED_ROW_STREAM_H_
